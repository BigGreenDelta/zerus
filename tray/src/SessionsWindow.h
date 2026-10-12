#pragma once
namespace AccountUsage {class Button;class RefreshButton;}
namespace SessionUsage {class ContextButton;}
namespace CacheStatus {class Chip;}
namespace RecoveryUi {class Panel;}
namespace ActivityWidth {class ColumnLayout;}
class ToolbarChip;
class SettingsPage;
class SwarmController;

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QWidget>
#include "FleetState.h"
#include "HgsClient.h"
#include "MessageComposer.h"
#include "SessionOrganization.h"

class QAction;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextBrowser;
class QStackedWidget;
class QSplitter;
class QMenu;
class ProjectsDialog;
class WorktreePanel;
class ActivityView;
class MessageComposer;
class QuestionCard;
class TerminalView;
class NativeUiView;
class OvermindModel;
class TasksView;
class QTabWidget;
class QFrame;
class SessionList;
class SessionPanelDock;
class QHBoxLayout;
class MachinesPage;
class MachineFilter;
class DashboardPage;
class AccountsPage;
class SessionSearch;
class IdentityBadge;
class SwarmButton;
class QToolBar;

// A view over hgs snapshots and its durable event journal. Closing this window
// never owns or terminates an agent. All operations still go through the CLI.
class SessionsWindow : public QWidget {
    Q_OBJECT
public:
    // `overmind`: the board model to read with, owned by the caller and outliving
    // the window (the tray's, which also raises Your Call notifications); the
    // window creates its own without one.
    explicit SessionsWindow(const QString &hgsPath, QWidget *parent = nullptr, OvermindModel *overmind = nullptr);
    void setFleet(const FleetState &fleet);
    void showSessionList();
    void showUpdates();
    void showSession(const QString &host, const QString &name);
    void showAttentionSession(const QString &host, const QString &name);
    // A Your Call notification: selects the session that asked (machine is its
    // Overmind node ID; never matched by name alone) and opens the Tasks tab on
    // the task's card in its project.
    void showDecision(const QString &project, const QString &task, const QString &machine, const QString &session);
    void showNotificationNotice(const QString &message) { showNotice(message, false); }
    void showProjects(const QString &host = {});
    void showNewSession(const QString &agent = {}, const QString &project = {}, const QString &host = {}, const QString &folder = {}, const QString &path = {}, const QString &account = {});
    void newSessionLaunchFailed(const QString &launchId, const QString &error = {});
    void setClipboardMode(bool clipboard);
    void setConnectionError(const QString &error);
    void setPollingHosts(const QSet<QString> &hosts);
    static QString status(const SessionInfo &session, bool reachable = true);

signals:
    void replyViewed(const QString &host, const QString &name, const QString &conversation, const QString &reply);
    void repliesMarkedRead(const QJsonObject &replies);
    void attentionMarksChanged(const QJsonObject &marks, const QJsonObject &readReplies);
    void refreshRequested();
    void sessionActivated(const QString &host, const QString &name, const QString &tty);
    void terminalOpenRequested(const QString &host, const QString &name, const QString &tty);
    void folderShellRequested(const QString &host, const QString &directory);
    void copySessionCommandRequested(const QString &host, const QString &name, const QString &archiveId);
    void copyTextRequested(const QString &text);
    void newSessionRequested(const QString &host, const QString &cmd, const QString &target, const QString &name, const QString &account, const QString &launchId);
    void projectsChanged(const QString &host);
    void accountLoginRequested(const QString &host, const QString &id);
    void accountInstallRequested(const QString &host, const QString &provider);
    void machineSshRequested(const QString &alias);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct Entry { QString host, machine, key, identity; SessionInfo session; bool online; };
    void applyTheme();
    void updateWindowPinAppearance();
    void applyContentScale();
    void applyActivityWidth();
    void saveOrganization();
    void savePendingLaunches();
    void placeLaunchedSessions();
    void createGroup(const QString &session = {});
    void showGroupMenu(const QString &group, const QPoint &position);
    void showWorkspaceSettings();
    void revealSession(const QString &key);
    void updateHeaderText();
    void rebuild();
    void selectSession();
    void inspect();
    void openSubagent(const QString &id);
    void closeSubagent();
    QJsonObject childRoster(const Entry &entry) const;
    bool showChild(const Entry &entry, const QString &id, const QJsonObject &child, bool countsComplete, bool includeCompleted) const;
    QString childKey(const Entry &entry, const QString &id) const;
    bool childNeedsAction(const Entry &entry) const;
    bool entryNeedsAttention(const Entry &entry) const;
    bool entryHasDraft(const Entry &entry) const;
    void renderDetails();
    // Pushes the projects and the selected session into the Tasks tab.
    void updateTasksView();
    // The Overmind node ID of an entry's machine; empty while unknown.
    QString overmindMachine(const Entry &entry) const;
    // Whether Overmind can name the entry's session: its machine is known, or
    // it is the machine of the replica read (machine then stays empty, which
    // the model and the CLI read as that machine).
    bool overmindKnown(const Entry &entry, QString *machine) const;
    // Overmind in the composer and the session list: the Tasks chip, what Add
    // as task may do for the selected session, and the rows' queue badges.
    void updateComposerTasks();
    void refreshOvermind();
    // Reads the boards the badges and the chip need: the projects of the
    // listed sessions and of the selected one, and the one the Tasks tab
    // shows. A project none of them needs is no longer read.
    void syncOvermindWatches();
    void addTask(const QString &key, const QString &text, const QList<MessageAttachment> &attachments,
                 MessageComposer::TaskDestination destination);
    void openSessionTasks();
    // A Your Call session placed by its Overmind machine (see showDecision).
    bool decisionSession(const QString &machine, const QString &session, QString *host) const;
    // A decision click whose session was not placed yet selects it when it can,
    // and opens the Tasks tab on the card when the click could not (shown).
    void resolvePendingDecision();
    void openDecision(const QString &project, const QString &task);
    struct PendingDecision { QString project, task, machine, session, selectedKey; bool shown = false; QElapsedTimer age; };
    PendingDecision m_pendingDecision;
    static constexpr qint64 PendingDecisionMs = 20000;
    void updateWorktrees();
    void filterFolder(const QString &host, const QString &path, bool checkout);
    void openRelatedSession(const QString &host, const QString &name, const QString &archive);
    void checkViewedReply();
    void markAllRepliesRead();
    void updateAttentionIndicator();
    void updateSessionsToggle();
    QString interruptedPrompt(const QString &key) const;
    void restoreInterruptedPrompt();
    struct InterruptedPrompt { QString run, conversation, text; };
    QHash<QString, InterruptedPrompt> m_interruptPrompts;
    void applySessionStrip();
    SessionPanelDock *m_sessionDock = nullptr;
    QPushButton *m_sessionsToggle = nullptr, *m_stripSearch = nullptr, *m_batchButton = nullptr, *m_panelNewSession = nullptr;
    QHBoxLayout *m_sessionHeader = nullptr, *m_filterRow = nullptr;
    QString m_countFull, m_countShort;
    bool m_focusSearch = false;
    int m_stripLayout = 0, m_stripReserve = 0;
    QWidget *m_stripSpacer = nullptr;
    QPushButton *m_savedDrafts = nullptr;
    QLabel *m_attentionBadge = nullptr;
    int m_attentionCount = 0;
    QTimer m_readTimer;
    QElapsedTimer m_readDwell;
    QString m_readCandidate, m_readSelectionKey;
    void acceptInspection(const QString &host, const QString &name, const QJsonObject &data, const QString &archiveId);
    void showNotice(const QString &message, bool error = false);
    void jumpToLatestActivity(QListWidgetItem *item);
    void openSession();
    void changeSession();
    void terminateSession();
    void archiveSession();
    // After the last session in a linked worktree is archived, offer (never run)
    // a review of that worktree for removal. Host, session, root, common dir.
    QStringList m_archivedWorktree;
    QPushButton *m_worktreeCleanup = nullptr;
    int m_worktreeCleanupRevision = 0;
    void offerWorktreeCleanup();
    void cleanUpArchivedWorktree();
    void renameSession();
    void forkSession();
    void populateBatchActions(QMenu *menu);
    enum class SelectionAction { MarkRead, ReviewLater, Pause, Resume, Archive, Forget, Terminate, Move };
    QList<Entry> selectionEntries() const;
    bool selectionActionApplies(const Entry &entry, SelectionAction action) const;
    static QString selectionActionText(SelectionAction action);
    static QString selectionActionIcon(SelectionAction action);
    void setupSelectionActions();
    void updateSelectionActions();
    void showSelectionMenu(const QPoint &position);
    void populateSelectionMenu(QMenu *menu, const QList<Entry> &entries);
    void populateSelectionProjects(QMenu *menu, const QList<Entry> &entries);
    void runSelectionAction(SelectionAction action, const QList<Entry> &entries, bool clearArchive = false);
    void runNextSelectionAction();
    bool finishSelectionAction(const QString &operation, bool ok, const QString &detail);
    void clearArchive();
    QFrame *m_selectionBar = nullptr;
    QLabel *m_selectionCount = nullptr;
    QToolBar *m_selectionTools = nullptr;
    QMap<SelectionAction, QAction *> m_selectionActions;
    QAction *m_clearArchiveAction = nullptr;
    QList<Entry> m_selectionQueue;
    SelectionAction m_selectionOperation = SelectionAction::Pause;
    QString m_selectionCommand;
    QStringList m_selectionErrors;
    int m_selectionTotal = 0, m_selectionDone = 0, m_selectionFailed = 0;
    bool m_selectionRunning = false;
    enum class SessionMenuAction { OpenTerminal, OpenFolder, FolderShell, CopyCommand, ChangeState, Rename, Fork, Archive, CopyName, CopyReference, CopyFolder, Terminate, MarkRead, ReviewLater, StartFresh };
    void copySessionReference(const Entry &entry, const QString &agent = {});
    void showSessionMenu(const QString &key, const QPoint &position);
    void clearContext();
    // A stopped session's draft waits for an explicit resume or fresh start.
    bool canStartStopped(const Entry &entry) const;
    void startFresh();
    void startToSend(const QString &key, const QString &text, const QList<MessageAttachment> &attachments);
    struct StartContinuation {
        QString key, host, name, run, conversation, text;
        QList<MessageAttachment> attachments;
        bool fresh = false, compact = false;
        qint64 started = 0;
    } m_startSend;
    void cancelStartContinuation(const QString &reason);
    void continueAfterStart(const QString &host, const QString &name, const QJsonObject &data);
    struct CompactContinuation {
        quint64 request = 0;
        QString key, host, name, run, conversation, text, retryId, commandId;
        QList<MessageAttachment> attachments;
        qint64 started = 0;
    };
    QHash<QString, CompactContinuation> m_compacts;
    QTimer m_compactTimer;
    QPushButton *m_compactCancel = nullptr;
    void cancelCompactContinuation(const QString &key, const QString &reason);
    void pollCompactContinuations();
    void continueAfterCompact(const QString &host, const QString &name, const QJsonObject &data);
    void compactToSend(const QString &key, const QString &host, const QString &name, const QString &run, const QString &conversation,
                       const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId = {});
    quint64 m_clearRequest = 0;
    QString m_clearKey;
    quint64 m_interruptRequest = 0;
    QString m_interruptKey;
    quint64 m_queueSendRequest = 0;
    QString m_queueSendKey;
    void runSessionMenuAction(const Entry &original, SessionMenuAction action);
    bool selectSessionEntry(const Entry &original);
    QString selectedDirectory() const;
    void openFolderShell();
    void openSessionFolder();
    void openLocalFolder(const QString &directory);
    const Entry *selected() const;
    bool isTerminating(const Entry &entry) const;
    void reconcileTerminations();
    bool archiveNameOccupied(const Entry &archive) const;
    QString messageBlockReason(const Entry &entry) const;
    QString modelSettingsBlockReason(const Entry &entry) const;
    void changeModelSettings(const QString &key, const QString &model, const QString &effort);
    void applyPendingModelSettings();
    void applyQueuedModelSettings();
    void sendMessage(const QString &key, const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId = {}, const QString &compactionId = {}, bool contextChosen = false);
    void deliverMessage(const QString &key, const QString &host, const QString &name, const QString &run, const QString &conversation,
                        const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId, const QString &compactionId);
    void messageAction(const QString &id, const QString &action);
    void finishMessage(quint64 request, bool ok, const QJsonObject &receipt, const QString &error = {}, bool uncertain = false);
    void reconcileMessages();
    void answerQuestion(const QString &key, const QString &questionId, const QJsonArray &answers);
    void finishQuestion(quint64 request, bool ok, const QString &error = {}, bool uncertain = false, bool submitted = false);
    void renderQuestion(const Entry &entry, int navigation = 0);
    void setInspectorVisible(bool visible);
    void updateInspectorMinimum();

    IdentityBadge *m_providerBadge, *m_machineBadge;
    HgsClient m_client;
    QString m_nativeLaunchToOpen;
    FleetState m_fleet;
    QList<Entry> m_entries;
    QJsonObject m_pendingLaunches;
    QHash<QString, Entry> m_terminating;
    QSet<QString> m_terminationFinished;
    QString m_terminationCommand;
    QString m_restoreKey, m_renameKey, m_forkKey, m_forkGroup, m_forkBefore;
    bool m_renameArchived = false;
    QString m_selectedKey, m_filter = "all";
    QString m_folderFilterHost,m_folderFilterPath;
    bool m_folderFilterCheckout=false;
    QPushButton *m_folderFilterClear=nullptr;
    WorktreePanel *m_worktrees=nullptr;
    QSet<QString> m_hostFilters;
    QString selectedHostTarget() const;
    void updateDashboard();
    QJsonObject m_details;
    QHash<QString, QString> m_questionSelections;
    QJsonArray m_events;
    qint64 m_cursor = 0;
    QString m_inspectError, m_connectionError, m_childrenHtml;
    quint64 m_settingsRequest = 0;
    QString m_settingsKey, m_settingsRun, m_settingsConversation;
    QString m_settingsHost, m_settingsName;
    bool m_settingsAutomatic = false;
    QSet<QByteArray> m_settingsAttempts;
    struct PendingSettings { QString host, name, run, conversation, model, effort, id; };
    QHash<QString, PendingSettings> m_queuedSettings;
    bool m_pending = false, m_clipboard = false, m_rebuilding = false;
    // Polls keep hidden pages' data current; these pages render when shown.
    bool m_sessionsStale = false, m_dashboardStale = false;
    bool m_draftRefreshPending = false;
    int m_tick = 0;
    QTimer m_timer;
    QElapsedTimer m_processPollAge,m_inspectionAge;
    SessionList *m_sessions;
    SessionSearch *m_searchResults;
    QStackedWidget *m_listStack;
    MachinesPage *m_machinesPage;
    MachineFilter *m_machineFilter;
    DashboardPage *m_dashboard;
    AccountsPage *m_accountsPage;
    SessionOrganization m_organization;
    SwarmController *m_swarm = nullptr;
    QByteArray m_savedOrganization;
    QPushButton *m_sessionsNav, *m_machinesNav, *m_accountsNav;
    SwarmButton *m_brand = nullptr;
    QString m_fullTitle, m_fullMeta;
    QList<QPushButton *> m_filters;
    QLineEdit *m_search;
    QLabel *m_heading, *m_count, *m_title, *m_meta, *m_badge, *m_model, *m_hint, *m_notice;
    QString m_modelElision;
    QLabel *m_connectionStatus;
    QPushButton *m_connectionRetry;
    QSet<QString> m_pollingHosts;
    void updateConnectionStatus();
    QPushButton *m_goal;
    ToolbarChip *m_markRead;
    class SessionFileDrop *m_fileDrop;
    QString terminalDropTarget() const;
    void dropTerminalFiles(const QStringList &paths);
    void pasteTerminalPaths(const QStringList &paths);
    quint64 m_terminalDropRequest = 0;
    QString m_terminalDropKey;
    int m_terminalDropCount = 0;
    SessionUsage::ContextButton *m_contextUsage, *m_subagentContextUsage;
    AccountUsage::Button *m_accountUsage;
    QWidget *m_usageStrip;
    AccountUsage::RefreshButton *m_usageRefresh;
    class AccountUsageStore *m_usageStore;
    QJsonObject m_accountUsageData;
    QLabel *m_usageWarning;
    ToolbarChip *m_usageLimit;
    QLabel *m_cacheWarning;
    QLabel *m_cacheDetail;
    QPushButton *m_cacheClear;
    CacheStatus::Chip *m_cacheStatus;
    void refreshAccountUsage(bool force = false);
    void renderAccountUsage();
    void updateDashboardAccounts(bool request = false, bool force = false);
    QPushButton *m_open, *m_shell, *m_fileManager, *m_pause, *m_more;
    QAction *m_renameAction, *m_forkAction, *m_clearAction, *m_freshAction, *m_archiveAction, *m_forgetAction;
    QAction *m_markAllReadAction = nullptr;
    QMenu *m_sessionMenu;
    QTextBrowser *m_children = nullptr, *m_info = nullptr;
    ActivityView *m_activityView = nullptr;
    class ProcessView *m_processes = nullptr;
    OvermindModel *m_overmind = nullptr;
    TasksView *m_tasks = nullptr;
    ToolbarChip *m_tasksChip = nullptr;
    QStringList m_listedProjects;   // projects of the sessions in the list
    bool m_overmindRefreshPending = false;
    // Add as task requests that failed (or had no answer in time), by a hash
    // of the request: the same request again reuses its request_id until it
    // succeeds, so the board replays a create that happened instead of adding
    // the task twice. Keyed by content, so a session rename does not lose it;
    // the oldest are forgotten first. Kept while the window lives.
    QHash<QByteArray, QString> m_failedTasks;
    QList<QByteArray> m_failedTaskOrder;
    QWidget *m_nativeSignIn = nullptr;
    ActivityView *m_subagentView = nullptr;
    QStackedWidget *m_activityStack = nullptr;
    QLabel *m_subagentTitle = nullptr, *m_subagentHint = nullptr;
    QString m_subagentId, m_subagentConversation, m_subagentCwd;
    QSet<QString> m_expandedSessions;
    QJsonObject m_subagentDetails;
    MessageComposer *m_composer = nullptr, *m_subagentComposer = nullptr;
    QHash<quint64, QString> m_childMessages;
    QuestionCard *m_question = nullptr;
    QList<ActivityWidth::ColumnLayout *> m_activityColumns;   // content sharing Activity's reading column
    RecoveryUi::Panel *m_recovery = nullptr;
    ToolbarChip *m_recoveryChip = nullptr;
    quint64 m_recoveryAction = 0;
    QString m_recoveryActionKey;
    TerminalView *m_terminal = nullptr;
    NativeUiView *m_nativeUi = nullptr;
    QTabWidget *m_detailTabs = nullptr;
    QTabWidget *m_inspector = nullptr;
    QLabel *m_detailsContext = nullptr;
    QLabel *m_detailsIdTitle = nullptr, *m_detailsIdValue = nullptr;
    QPushButton *m_copySessionId = nullptr;
    QPushButton *m_detailsClear = nullptr;
    QFrame *m_inspectorPanel = nullptr;
    QSplitter *m_workSplitter = nullptr;
    QPushButton *m_inspectorToggle = nullptr;
    int m_inspectorWidth = 290;
    struct PendingMessage { QString key, runId, conversationId; };
    QHash<quint64, PendingMessage> m_pendingMessages;
    QHash<QString, QJsonArray> m_localMessages;
    struct PendingAnswer { QString key, questionId, questionHash; };
    QHash<quint64, PendingAnswer> m_pendingAnswers;
    bool m_confirmingQuestionCache = false;
    QStackedWidget *m_detailStack;
    QSplitter *m_splitter;
    QStackedWidget *m_pages;
    SettingsPage *m_settingsPage;
    QPushButton *m_settingsNav;
    QPushButton *m_windowPin = nullptr;
    ProjectsDialog *m_projectsPage;
    QPushButton *m_projectsNav;
    bool m_dark = false;
    QString m_fg, m_muted, m_surface, m_border, m_accent;
};
