#pragma once

#include <QWidget>
#include <QHash>
#include "FleetState.h"
#include "AccountUsage.h"

class QLabel;
class QPushButton;
class QGridLayout;
class DashboardMachineCard;
class DashboardSessionRow;
class DashboardAccountCard;

class DashboardPage : public QWidget {
    Q_OBJECT
public:
    explicit DashboardPage(QWidget *parent = nullptr);
    void setFleet(const FleetState &fleet, const QSet<QString> &drafts = {});
    void setTheme(bool dark);
    void setAccounts(const QJsonArray &profiles);
    void setUsageIndicator(AccountUsage::Indicator indicator);
signals:
    void sessionRequested(const QString &host, const QString &name);
    void machinesRequested(const QString &host);
    void filterRequested(const QString &host, const QString &filter);
    void refreshRequested();
    void accountRequested(const QString &host, const QString &id);
    void accountsRequested();
protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void render();
    void arrangeMachines();
    void arrangeSessions();
    void arrangeAccounts();
    FleetState m_fleet;
    QSet<QString> m_drafts;
    QLabel *m_inactive, *m_empty;
    QPushButton *m_active, *m_attention, *m_connected, *m_sessions;
    QGridLayout *m_machineGrid;
    QGridLayout *m_sessionGrid;
    QGridLayout *m_accountGrid;
    QLabel *m_accountsEmpty;
    QHash<QString,DashboardAccountCard *> m_accountCards;
    AccountUsage::Indicator m_usageIndicator = AccountUsage::indicator();
    QStringList m_accountOrder;
    int m_accountColumns = 0;
    QHash<QString, DashboardMachineCard *> m_machineCards;
    QHash<QString, DashboardSessionRow *> m_rows;
    QStringList m_machineOrder, m_sessionOrder;
    int m_columns = 0;
    int m_sessionColumns = 0;
    bool m_dark = true;
};
