#pragma once
#include "SessionGraphStore.h"
#include <QObject>
#include <QSet>
#include <QTimer>

// One bounded read at a time across configured routes. Transport is injected;
// cache parsing, validation and persistence run on a dedicated worker pool.
class GraphController : public QObject {
    Q_OBJECT
  public:
    explicit GraphController(QString cachePath = {}, QObject *parent = nullptr);
    void setRoutes(const QSet<QString> &routes);
    void setActive(bool active);
    void refresh();
    void received(quint64 request, const QString &route, const QJsonObject &page);
    void failed(quint64 request, const QString &route, const QString &error);
    const SessionGraphStore &store() const { return m_store; }
    bool confirmed(const QString &route) const;
    QString summary() const;
    QString details() const;
    bool pending() const { return m_pending != 0 || m_working || m_loading; }
  signals:
    void requestPage(quint64 request, const QString &route, bool changes, const QString &cursor);
    void projectionChanged();
    void statusChanged();

  private:
    struct Route {
        qint64 due = 0, lastSuccess = 0;
        bool confirmed = false, snapshot = false;
        QString cursor, error;
        int resets = 0;
    };
    void tick();
    void persist();
    void prune();
    QTimer m_timer, m_save;
    SessionGraphStore m_store;
    QHash<QString, Route> m_routes;
    QString m_cachePath, m_route, m_cursor, m_cacheError, m_lastRoute;
    quint64 m_sequence = 0, m_pending = 0, m_generation = 0, m_requestGeneration = 0;
    qint64 m_started = 0;
    bool m_configured = false, m_haveLastRoute = false;
    bool m_active = false, m_working = false, m_loading = false, m_changes = false;
};
