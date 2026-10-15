#pragma once
#include <QWidget>
#include <QMap>
#include <QJsonObject>
#include "HgsClient.h"
class QTimer; class QComboBox; class QDateEdit; class QLabel; class QPushButton;

// Owns only explicitly requested archive pages; regular fleet polls never append.
class ArchiveBrowser : public QWidget {
    Q_OBJECT
public:
    explicit ArchiveBrowser(HgsClient &client, QWidget *parent = nullptr);
    void configure(const QMap<QString,QString> &hosts, bool active);
    QList<SessionInfo> sessions(const QString &host) const;
    bool includes(double archivedAt) const;
    void refresh();
    void setQuery(const QString &query);
    int total() const;
    bool totalsKnown() const;
    bool hasMore() const;
    bool paged() const { return !m_hosts.isEmpty(); }
signals:
    void changed();
private:
    struct Page { QList<SessionInfo> rows; QString cursor,error; quint64 request=0; int total=0; bool loaded=false; };
    void loadMore();
    void updateStatus();
    QJsonObject query(const QString &cursor) const;
    HgsClient &m_client;
    QComboBox *m_period; QDateEdit *m_from,*m_until; QLabel *m_status; QPushButton *m_more,*m_refresh;
    QMap<QString,QString> m_hosts;
    QMap<QString,Page> m_pages;
    bool m_active=false;
    QString m_query;
    QTimer *m_debounce;
};
