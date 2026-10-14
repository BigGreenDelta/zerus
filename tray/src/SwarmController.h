#pragma once
#include <QObject>
#include <QJsonObject>
#include <QTimer>
#include "HgsClient.h"

// Local CLI reads only; the HGS worker owns network polling. Writes are serialized
// and retain the versions the user actually saw, including edits during a read.
class SwarmController : public QObject {
    Q_OBJECT
public:
    explicit SwarmController(HgsClient *client, QObject *parent = nullptr);
    void start(const QJsonObject &organization);
    void edit(const QJsonObject &organization);
    void refresh();
    QJsonObject snapshot() const { return m_snapshot; }
    bool busy() const { return m_request != 0; }
    bool ready() const { return m_ready; }
    bool settled() const { return m_ready && !busy() && m_base == m_desired; }
    void suspend(bool paused) { m_suspended = paused; if(!paused)refresh(); }
    void acceptExternal(const QJsonObject &snapshot);
    void setDraftEditing(bool active) { m_draftEditing = active; if(!active)refresh(); }
    // node: this computer's catalog ID (the snapshot's node_id), whose local
    // Windows folders are merged back from local.
    static QJsonObject presentation(const QJsonObject &shared, const QJsonObject &local, const QString &node = {});
signals:
    void organizationReady(const QJsonObject &organization);
    void statusChanged(const QString &text, bool error);
    void snapshotChanged(const QJsonObject &snapshot);
private:
    void send(const QString &action);
    void received(quint64 request, const QJsonObject &result);
    void persist();
    HgsClient *m_client;
    QTimer m_poll, m_flush;
    quint64 m_request = 0;
    QString m_action;
    bool m_ready = false, m_started = false, m_suspended = false, m_draftEditing = false;
    QJsonObject m_base, m_desired, m_sent, m_versions, m_snapshot;
    // This computer's catalog ID from the latest result, whose local Windows folders
    // requests leave out; empty until one arrives.
    QString m_node;
    QByteArray m_persistedOrganization, m_persistedVersions;
};
