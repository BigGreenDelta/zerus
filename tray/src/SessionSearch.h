#pragma once
#include "FleetState.h"
#include <QTimer>
#include <QWidget>
class QListWidget;
class QListWidgetItem;
class QLabel;
class QPushButton;

// Search requests stay on each session's machine. Only matching public excerpts
// cross SSH; old asynchronous responses never replace a newer query.
class SessionSearch : public QWidget {
    Q_OBJECT
public:
    SessionSearch(const QString &hgs, QWidget *parent = nullptr);
    void setQuery(const QString &query);
    void setScope(const FleetState &fleet, const QString &filter, const QString &host, const QSet<QString> &drafts = {});
    void setHostsScope(const FleetState &fleet, const QString &filter, const QSet<QString> &hosts, const QSet<QString> &drafts = {});
    void setProjectScope(const QSet<QString> &sessionKeys, bool enabled);
    void setTheme(bool dark);
    void setFolderScope(const QString &host,const QString &path,bool checkout);
    bool active() const { return !m_query.isEmpty(); }
    // The workspace key of a result row's session (with its archive id for an archive).
    static QString sessionKey(const QListWidgetItem *item);
signals:
    void archiveSessionsReady(const QString &host, const QJsonArray &rows);
    void resultActivated(const QString &host, const QString &name, const QString &archive,
                         const QString &run, const QJsonObject &event, const QString &query);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void request();
    void rebuild();
    bool accepts(const QString &host, const SessionInfo &session, bool online) const;
    void activateResult(QListWidgetItem *item);
    HgsClient m_client;
    FleetState m_fleet;
    QString m_query, m_filter, m_selectedHit;
    QSet<QString> m_hosts, m_projectKeys;
    bool m_projectScope = false;
    QSet<QString> m_drafts;
    QString m_folderHost,m_folderPath;bool m_folderCheckout=false;
    QString m_pressedCurrentKey;
    QTimer m_debounce;
    QListWidget *m_list;
    QLabel *m_status;
    QPushButton *m_refresh;
    QHash<QString, QPair<quint64, QString>> m_pending;
    QHash<QString, QJsonObject> m_replies;
    QHash<QString, QString> m_errors;
    bool m_dark = false;
};
