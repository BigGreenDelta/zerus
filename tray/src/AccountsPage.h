#pragma once
#include <QWidget>
#include <QHash>
#include <QJsonObject>
#include "FleetState.h"
class QComboBox; class QListWidget; class QLabel; class QPushButton; class QLayout; class QStackedLayout;
class QTreeWidget;
class BusyIndicator;
class IdentityBadge;
class AccountUsageStore;
namespace AccountUsage {class Panel; class RefreshButton;}
class AccountsPage : public QWidget {
    Q_OBJECT
public:
    explicit AccountsPage(const QString &hgsPath, QWidget *parent = nullptr, AccountUsageStore *usage = nullptr);
    void setFleet(const FleetState &fleet);
    void showMachine(const QString &host);
    void showAccount(const QString &host, const QString &id);
    void reload();
    void ensureCatalogs(bool force = false);
    QJsonArray profiles(bool includeRemoved = false) const;
    void setTheme(bool dark);
    void refreshAppearance();
signals:
    void loginRequested(const QString &host, const QString &id);
    void installRequested(const QString &host, const QString &provider);
    void accountsChanged();
    void catalogChanged();
    void notice(const QString &message, bool error = false);
protected:
    void showEvent(QShowEvent *) override;
private:
    void render();
    void selectionChanged();
    void add();
    void copy(const QString &target = QStringLiteral("*"));
    void setupDeepSeek(const QString &host, const QString &label = {});
    void updateRefreshState();
    int pendingAccounts() const;
    void remove();
    void useSavedProfile(const QJsonObject &profile);
    QJsonArray savedProfiles(const QString &host, const QString &provider) const;
    QJsonArray savedForAccount(const QString &host) const;
    void rename();
    void permissions();
    void inspectSelected(bool refresh = false);
    void inspectProfiles(bool refresh = false);
    void renameNext();
    void request(const QString &host, const QStringList &args);
    QJsonObject selected() const;
    QJsonObject selectedAccount() const;
    bool online(const QString &host) const;
    void renderMachines();
    HgsClient m_client;
    FleetState m_fleet;
    QComboBox *m_machine, *m_profileChoice;
    QListWidget *m_list;
    QStackedLayout *m_bodyStack;
    BusyIndicator *m_loadingSpinner, *m_summarySpinner;
    QJsonArray m_displayProfiles;
    bool m_hasSnapshot = false;
    IdentityBadge *m_providerBadge;
    QLabel *m_summary, *m_title, *m_detail, *m_identity, *m_permissionState;
    QPushButton *m_add, *m_login, *m_copy, *m_remove, *m_rename, *m_install, *m_permissions, *m_default, *m_startOpenCode;
    // OpenCode's connected providers and their notes, read-only text from that machine's catalog.
    QTreeWidget *m_openCodeProviders;
    QLabel *m_openCodeProvidersNote;
    AccountUsage::RefreshButton *m_refresh;
    QLayout *m_machineTags;
    QWidget *m_machineCard;
    AccountUsage::Panel *m_usage;
    AccountUsageStore *m_usageStore;
    QHash<QString,qint64> m_catalogChecked;
    QString m_requestedProfile;
    QString m_activeProfile, m_machineSignature;
    QList<QJsonObject> m_renameQueue;
    QString m_renameLabel;
    bool m_renderQueued = false, m_dark = false;
    QHash<QString, QJsonObject> m_catalogs;
    QHash<quint64, QPair<QString, QString>> m_requests;
    QString m_loginAfterAddHost, m_loginAfterAddId;
    // One machine explicitly opened from Machines, including peers disabled for polling.
    QString m_explicitHost;
};
