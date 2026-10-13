#pragma once
#include <QDialog>
#include <QMap>
#include "FleetState.h"
#include "SessionOrganization.h"
class QComboBox;
class QCheckBox;
class QToolButton;
class QLabel;
class QLineEdit;
class QPushButton;
class QAction;
class QListWidget;
class AccountUsageStore;

class NewSessionDialog : public QDialog {
    Q_OBJECT
public:
    NewSessionDialog(const QString &hgsPath,const FleetState &fleet,const QString &host = {},
                     const QString &agent = {},const QString &project = {},QWidget *parent = nullptr,
                     AccountUsageStore *usage = nullptr);
    void setFleet(const FleetState &fleet);
    void setGroups(const SessionOrganization &organization,const QString &selectedProject = {});
    bool selectFolder(const QString &folderId);
    void selectPath(const QString &path, bool automaticWorktree = true);
    void prefillSessionFolder(const QString &path, const QString &canonicalPath = {});
    void selectAccount(const QString &account);
    void reject() override;
signals:
    // addFolder: the confirmed folder outside the project that joins it once the
    // session starts, or empty. A worktree generated from it never replaces it.
    void launchRequested(const QString &host,const QString &agent,const QString &target,const QString &name,const QString &account,const QString &project,bool openTerminal,const QString &addFolder,const QString &model);
    void manageProjectsRequested(const QString &host);
    void projectFoldersRequested(const QString &project);
private:
    void loadMachines();
    void loadFolders(const QString &selectedPath = {});
    void browseFolder();
    void updateWorktrees();
    QString matchingProjectFolder(const QString &path, const QString &canonicalPath = {}) const;
    void finishFolderPrefill(const QJsonObject &catalog);
    void chooseWorktree();
    void createWorktree();
    bool automaticWorktreeAvailable() const;
    QString automaticWorktreePath() const;
    bool projectWorktree(const QString &path, const QJsonObject &catalog) const;
    bool projectWorktree(const QString &path) const;
    void loadAccounts(bool refresh = false);
    void updateAccounts();
    QJsonObject cachedUsage(const QJsonObject &profile) const;
    void updateUsage();
    void updateModels();
    QString agent() const;
    QString account() const;
    void updateForm();
    void start();
    void launch(const QString &target,const QString &addFolder);
    QString host() const;
    HgsClient m_client;
    AccountUsageStore *m_usageStore;
    FleetState m_fleet;
    SessionOrganization m_projects;
    QString m_initialProject,m_initialHost,m_accountHost;
    QString m_preferredAccount;
    QString m_preferredAgent;
    QStringList m_agents;
    bool m_accountPreset=false;
    bool m_accountCatalogLoaded=false;
    QComboBox *m_project,*m_machine,*m_folder,*m_model;
    QListWidget *m_account;
    QPushButton *m_refreshAccounts;
    QLabel *m_accountStatus;
    QCheckBox *m_openTerminal;
    QMap<QString, QString> m_browsedFolders;
    QString m_folderContext;
    QLineEdit *m_name;
    QLabel *m_projectPath,*m_preview,*m_error,*m_nameError,*m_modelLabel;
    QPushButton *m_start,*m_browse,*m_autoWorktree,*m_plainFolder,*m_worktreeOptions,*m_cancel;
    QAction *m_worktrees,*m_newWorktree;
    QLabel *m_worktreeTarget;
    QString m_generatedBranch;
    bool m_autoPreferred=true,m_creationFailed=false,m_startWithWorktree=false;
    quint64 m_creation=0;
    QJsonObject m_creationCatalog;
    QString m_creationFolder;
    QJsonObject m_worktreeCatalog;
    QMap<QString, QJsonObject> m_verifiedWorktrees;
    QString m_worktreeContext;
    quint64 m_catalogRequest=0;
    quint64 m_prefillRequest=0;
    QString m_prefillPath,m_prefillCanonicalPath,m_prefillContext;
    QToolButton *m_manage;
    quint64 m_validation = 0,m_accountRequest = 0;
    QJsonArray m_accounts;
};
