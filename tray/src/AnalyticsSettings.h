#pragma once
#include "AnalyticsClient.h"
#include "AnalyticsData.h"
#include <QWidget>
#include <QMap>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class AnalyticsSettings : public QWidget {
public:
    explicit AnalyticsSettings(const QString &executable, QWidget *parent = nullptr);
    void setPeers(const QStringList &peers);
    void refresh();
private:
    void nextComputer();
    void render();
    void renderRows();
    void showDetail();
    void configure();
    void purge();
    void exportDiagnostics();
    void controls();
    QList<AnalyticsData::Observation> observations() const;
    QString displayRoute(const QString &route) const;
    AnalyticsClient m_client;
    QComboBox *m_computer, *m_provider;
    QDateEdit *m_from, *m_through;
    QCheckBox *m_enabled;
    QSpinBox *m_retention;
    QPushButton *m_refresh, *m_apply, *m_purge, *m_export, *m_previous, *m_next;
    QLabel *m_status, *m_overview, *m_page;
    QPlainTextEdit *m_coverage, *m_detail, *m_voice;
    QTableWidget *m_table;
    QMap<QString,AnalyticsData::Observation> m_snapshots;
    AnalyticsData::EpochGuard m_epochs;
    QStringList m_peers, m_routes, m_queue;
    QList<QJsonObject> m_rows;
    QJsonObject m_local;
    QDate m_appliedFrom, m_appliedThrough;
    bool m_busy = false, m_mutating = false;
    bool m_peerLimit = false;
    int m_offset = 0;
};
