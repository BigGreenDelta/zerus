#pragma once
#include <QDialog>
#include <QHash>
#include <QMap>
#include "FleetState.h"
#include "MachinePlatform.h"
#include "SessionOrganization.h"
class QComboBox;
class QCheckBox;
class QToolButton;
class QLabel;
class QLineEdit;
class QPushButton;
class QAction;
class QFrame;
class QListWidget;
class AccountUsageStore;
class OpenCodeModelPicker;
class RepositoryClone;
class QFormLayout;
class QProgressBar;
class QStackedWidget;

class NewSessionDialog : public QDialog {
    Q_OBJECT
public:
    NewSessionDialog(const QString &hgsPath,const FleetState &fleet,const QString &host = {},
                     const QString &agent = {},const QString &project = {},QWidget *parent = nullptr,
                     AccountUsageStore *usage = nullptr, SessionOrganization *projects = nullptr);
    void setFleet(const FleetState &fleet);
    void setGroups(const SessionOrganization &organization,const QString &selectedProject = {});
    bool selectFolder(const QString &folderId);
    void selectPath(const QString &path, bool automaticWorktree = true);
    void prefillSessionFolder(const QString &path, const QString &canonicalPath = {});
    void selectAccount(const QString &account);
    void setProjectExplicit(bool explicitChoice) { m_projectExplicit = explicitChoice; }
    bool implicitInbox() const;
    void setProjectCatalogSwarm(const QString &swarm) { m_projectCatalogSwarm = swarm; }
    void reject() override;
signals:
    // addFolder: the confirmed folder outside the project that joins it once the
    // session starts, or empty. A worktree generated from it never replaces it.
    // effort: the chosen OpenCode model's variant, or empty for that model's default.
    void launchRequested(const QString &host,const QString &agent,const QString &target,const QString &name,const QString &account,const QString &project,bool openTerminal,const QString &addFolder,const QString &model,const QString &effort);
    void manageProjectsRequested(const QString &host);
    void projectFoldersRequested(const QString &project);
    // Open Machines with this computer selected (offered without a computer).
    void machinesRequested();
    void organizationChanged();
private:
    void loadMachines();
    void loadFolders(const QString &selectedPath = {});
    void browseFolder();
    void setRepositorySource(bool enabled);
    void syncRepositoryComputer();
    void editRepositorySource();
    void useRepository(const QString &path);
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
    void requestUsage(bool force);
    QJsonObject cachedUsage(const QJsonObject &profile) const;
    QJsonObject accountUsage(const QJsonArray &members) const;
    void refreshUsage();
    void updateUsage();
    void updateModels();
    void updateEfforts();
    // The selected OpenCode account row of this computer's catalog, if any.
    QJsonObject openCodeProfile() const;
    void updateOpenCodeService();
    void startOpenCode();
    // OpenCode's grouped picker for the selected computer and folder.
    QString modelKey() const;
    void updateModelButton();
    void openModelPicker();
    void requestFolderCatalog(bool refresh);
    void storeFolderCatalog(const QString &key,const QJsonObject &answer);
    void chooseModel(const QString &value);
    void changeFavorite(const QString &value,bool favorite);
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
    SessionOrganization *m_liveProjects=nullptr;
    bool m_cloneLaunchPending=false,m_repositorySource=false,m_customDestination=false,m_customProjectName=false,m_repositoryNewProject=false;
    RepositoryClone *m_repository=nullptr;
    MachinePlatform::Style m_repositoryStyle=MachinePlatform::Style::Posix;
    QFormLayout *m_form;
    QStackedWidget *m_sourceInput,*m_sourceDetails,*m_newProjectField,*m_newProjectLabel;
    QLabel *m_sourceLabel,*m_destinationLabel;
    QWidget *m_folderField,*m_repositoryDestinationField,*m_repositoryState;
    QPushButton *m_folderSource,*m_gitSource,*m_repositoryBrowse,*m_repositoryCheck,*m_repositoryAnother;
    QLineEdit *m_repositoryUrl,*m_repositoryDestination,*m_newProjectName;
    QLabel *m_repositoryStatus;
    QProgressBar *m_repositoryProgress;
    QString m_initialProject,m_initialHost,m_accountHost,m_projectCatalogSwarm;
    QString m_preferredAccount;
    QString m_preferredAgent;
    QStringList m_agents;
    bool m_accountPreset=false;
    bool m_projectExplicit=false;
    bool m_accountCatalogLoaded=false;
    // Rows group the catalog when it arrives; later limits never regroup them.
    bool m_regroupAccounts=true,m_forceUsage=false;
    MachinePlatform::Style m_accountStyle=MachinePlatform::Style::Posix;
    QJsonArray m_accountGroups;
    QComboBox *m_project,*m_machine,*m_folder,*m_model,*m_effort;
    QListWidget *m_account;
    QPushButton *m_refreshAccounts;
    QLabel *m_accountStatus;
    QCheckBox *m_openTerminal;
    QMap<QString, QString> m_browsedFolders;
    QString m_folderContext;
    QLineEdit *m_name;
    QLabel *m_projectPath,*m_preview,*m_error,*m_nameError,*m_modelLabel,*m_effortLabel,*m_catalogStatus;
    QWidget *m_openCodeService;
    QPushButton *m_startOpenCode;
    quint64 m_serviceStart=0;
    QString m_serviceHost;
    // The hidden m_model keeps the selected model and its variants; this button
    // names it and opens the picker.
    QPushButton *m_modelButton;
    QFrame *m_modelPopup;
    OpenCodeModelPicker *m_modelPicker;
    // `account models` answers per computer and folder, at most four: ready
    // catalogs add their models; other states and failures are shown again
    // instead of asked again.
    QHash<QString, QJsonObject> m_folderCatalogs;
    QStringList m_folderOrder;
    QHash<quint64, QString> m_folderRequests;
    // The newest request issued per folder, while any request of it is pending.
    QHash<QString, quint64> m_folderLatest;
    // The picker's context (computer and folder while OpenCode is chosen), the
    // catalog it shows, and the last choice made in it with its computer and folder.
    QString m_modelContext,m_chosenHost,m_chosenKey,m_favoriteError,m_pickerAvailability;
    QJsonObject m_shownCatalog,m_chosenModel;
    struct FavoriteChange {QString key,value;bool favorite=false;};
    QHash<quint64, FavoriteChange> m_favoriteRequests;
    QPushButton *m_start,*m_browse,*m_autoWorktree,*m_plainFolder,*m_worktreeOptions,*m_cancel,*m_openMachines;
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
