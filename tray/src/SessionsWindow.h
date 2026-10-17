#pragma once
namespace AccountUsage {class Button;class RefreshButton;}
namespace SessionUsage {class ContextButton;}
namespace CacheStatus {class Chip;}
namespace RecoveryUi {class Panel;}
namespace ActivityWidth {class ColumnLayout;}
class ArchiveBrowser;
class ToolbarChip;
class SettingsPage;
class SwarmController;

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QWidget>
#include <optional>
#include "FleetState.h"
#include "HgsClient.h"
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
class QTabWidget;
class QFrame;
class SessionList;
class SessionPanelDock;
class QHBoxLayout;
class MachinesPage;
class MachineFilter;
class ProjectFilter;
class DashboardPage;
class AccountsPage;
class GoToDialog;
class ShortcutsDialog;
struct GoToItem;
class SessionSearch;
class IdentityBadge;
class SwarmButton;
class QToolBar;
class BusyIndicator;
class ShortcutRouter;

// A view over hgs snapshots and its durable event journal. Closing this window
// never owns or terminates an agent. All operations still go through the CLI.
class SessionsWindow : public QWidget {
    Q_OBJECT
public:
    explicit SessionsWindow(const QString &hgsPath, QWidget *parent = nullptr);
    void setFleet(const FleetState &fleet);
    void showSessionList();
    void showUpdates();
    void showSession(const QString &host, const QString &name);
    void showAttentionSession(const QString &host, const QString &name);
    void showNotificationNotice(const QString &message) { showNotice(message, false); }
    void showProjects(const QString &host = {});
    void showNewSession(const QString &agent = {}, const QString &project = {}, const QString &host = {}, const QString &folder = {}, const QString &path = {}, const QString &account = {});
    void newSessionLaunchFailed(const QString &launchId, const QString &error = {});
    void setClipboardMode(bool clipboard);
    void setConnectionError(const QString &error);
    void setPollingHosts(const QSet<QString> &hosts);
    static QString status(const SessionInfo &session, bool reachable = true);
    // Tests only, so suites need not wait out fixed delays: information notices
    // (5 s), compaction polls (2.5 s), the details poll tick (500 ms) with its
    // Activity interval (2.5 s), and the wait of Send and push now (20 s, which
    // also restarts an armed push's wait). The application keeps the defaults.
    void setNoticeDurationForTests(int ms) { m_noticeMs = ms; }
    void setCompactPollIntervalForTests(int ms) { m_compactTimer.setInterval(ms); }
    void setDetailsPollForTests(int tickMs, int activityMs) { m_timer.setInterval(tickMs); m_activityPollMs = activityMs; }
    void setPushTimeoutForTests(int ms) { m_pushTimeout = ms; if (m_push) m_push->deadline.setRemainingTime(ms); }

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
    ArchiveBrowser *m_archiveBrowser = nullptr;
    QMap<QString,QList<SessionInfo>> m_archiveSearchRows;
    struct Entry { QString host, machine, key, identity; SessionInfo session; bool online; };
    void applyTheme();
    void updateWindowPinAppearance();
    void applyContentScale();
    void applyActivityWidth();
    void saveOrganization();
    void savePendingLaunches();
    void placeLaunchedSessions();
    bool takeEndedLaunch(const QString &launchId, QJsonObject *launch = nullptr);
    void createGroup(const QString &session = {});
    void showGroupMenu(const QString &group, const QPoint &position);
    void showWorkspaceSettings();
    void revealSession(const QString &key);
    // The listed session a saved draft belongs to, or null.
    const Entry *draftOwner(const QString &draftKey) const;
    void openDraftSession(const QString &draftKey);
    void updateHeaderText();
    void updateSessionLoading();
    void updateSessionFeedback();
    void rebuild();
    QMap<int, QVariant> sessionCardRoles(const Entry &entry) const;
    QMap<int,QVariant> childCardRoles(const Entry &entry,const QString &id,const QJsonObject &child) const;
    void selectSession();
    void inspect();
    // The DeepSeek native UI: its tab when the embedded plugin is available,
    // otherwise the system browser (NativeBrowserPlugin.h).
    void showNativeUi(const Entry &entry);
    void openSubagent(const QString &id);
    void closeSubagent();
    QJsonObject childRoster(const Entry &entry) const;
    bool showChild(const Entry &entry, const QString &id, const QJsonObject &child, bool countsComplete, bool includeCompleted) const;
    QString childKey(const Entry &entry, const QString &id) const;
    bool childNeedsAction(const Entry &entry) const;
    bool entryNeedsAttention(const Entry &entry) const;
    bool entryHasDraft(const Entry &entry) const;
    void renderDetails();
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
    void clearNotice();
    // Opens a session file like a browser download; see ExternalFile.h.
    void openExternalFile(const QString &path, bool otherMachine);
    void jumpToLatestActivity(QListWidgetItem *item);
    void openSession();
    void changeSession();
    // Spec D5: asks before resuming or restoring this selected session starts its
    // agent in a session host of another Windows session. False after Cancel, or
    // when the selection changed while the warning was open.
    bool confirmLocalStart(const Entry &original);
    void terminateSession();
    void archiveSession();
    // Native lifecycle cleanup reports its result after Archive or Terminate.
    // Keep a review action for remaining work. Host, session, root, common dir.
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
    enum class SessionMenuAction { OpenTerminal, OpenProcesses, OpenFolder, FolderShell, CopyCommand, ChangeState, Rename, Fork, Archive, CopyName, CopyReference, CopyFolder, Terminate, MarkRead, ReviewLater, StartFresh };
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
    QString m_interruptName, m_interruptMachine;
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
                        const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId, const QString &compactionId, bool fullContextConsent = false, const QJsonObject &evidence = {});
    void messageAction(const QString &id, const QString &action);
    void finishMessage(quint64 request, bool ok, const QJsonObject &receipt, const QString &error = {}, bool uncertain = false);
    void reconcileMessages();
    void answerQuestion(const QString &key, const QString &questionId, const QJsonArray &answers);
    void finishQuestion(quint64 request, bool ok, const QString &error = {}, bool uncertain = false, bool submitted = false);
    void renderQuestion(const Entry &entry, int navigation = 0);
    void setInspectorVisible(bool visible);
    void updateInspectorMinimum();
    // Keyboard commands of the Window scope (Shortcuts.h), implemented in
    // SessionsWindowShortcuts.cpp. Every command's action stays enabled; a
    // command with nothing to do does nothing or shows a notice.
    void setupShortcuts();
    void applyShortcuts();
    void runShortcut(const QString &id);
    QAction *shortcutAction(const QString &id) const;
    // Empty queueId: the selected session's current snapshot. False, with a
    // notice when nothing can be sent, unless a request is already running.
    bool sendQueuedNow(const QString &queueId = {});
    // Go to and the cheat sheet, in SessionsWindowGoTo.cpp.
    void openGoTo();
    void openShortcutHelp();
    QList<GoToItem> goToItems() const;
    void activateGoTo(const GoToItem &item);
    void rememberRecentSession(const QString &key);
    GoToDialog *m_goTo = nullptr;
    ShortcutsDialog *m_shortcutHelp = nullptr;
    QStringList m_recentSessions;
    void stepSession(int direction, bool attention);
    void stepTab(int direction);
    void stepFocus(int direction);
    void stepContentScale(int direction);
    QWidget *viewFocusTarget() const;
    QHash<QString, QAction *> m_shortcutActions;
    ShortcutRouter *m_shortcutRouter = nullptr;
    // Send and push now (message.push): after its send is confirmed, the first
    // snapshot of this session's native queue that holds the message is sent now.
    // Run and conversation are those its send request expects.
    struct PendingPush { QString key, run, conversation, text; quint64 request = 0; bool confirmed = false; QDeadlineTimer deadline; };
    std::optional<PendingPush> m_push;
    QTimer m_pushTimer;
    int m_pushTimeout = 20000;
    void armPush(const QString &key, const QString &text);
    void pushSendFinished(quint64 request, bool ok, bool uncertain);
    void pushInspected(const QString &key, const QJsonObject &details);
    // Before another session (or none) becomes the selected one.
    void pushSelectionChanged(const QString &key);
    void checkPush();
    void endPush(const QString &notice = {}, bool error = false);

    IdentityBadge *m_providerBadge, *m_machineBadge;
    HgsClient m_client;
    QString m_nativeLaunchToOpen;
    FleetState m_fleet;
    QList<Entry> m_entries;
    QJsonObject m_pendingLaunches;
    // Launches a listing reported ended, oldest first, with their launch intent.
    QList<QPair<QString, QJsonObject>> m_endedLaunches;
    QHash<QString, Entry> m_terminating;
    QSet<QString> m_terminationFinished;
    QString m_terminationCommand;
    QString m_restoreKey, m_renameKey, m_forkKey, m_forkGroup, m_forkBefore;
    bool m_renameArchived = false;
    QString m_selectedKey, m_filter = "all";
    bool m_initialSelectionPending = true;
    QString m_folderFilterHost,m_folderFilterPath;
    bool m_folderFilterCheckout=false;
    QPushButton *m_folderFilterClear=nullptr;
    WorktreePanel *m_worktrees=nullptr;
    QSet<QString> m_hostFilters, m_projectFilters;
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
    static QByteArray settingsIdentity(const QString &key, const PendingSettings &pending);
    const SessionInfo *listedSettingsSession(const PendingSettings &pending) const;
    bool settingsAttemptAllowed(const QString &key, const PendingSettings &pending, const QByteArray &identity) const;
    void requestAutomaticSettings(const QString &key, const PendingSettings &pending, const QByteArray &identity);
    // An automatic apply that dispatch scheduled instead, as when a chooser
    // appeared after the idle listing, by session key. It runs once more after
    // the listing or inspect shows the session leave Ready (`rearmed`).
    struct ScheduledSettings { QByteArray identity; bool rearmed = false; };
    QHash<QString, ScheduledSettings> m_scheduledSettings;
    // The selected session's inspection was already running when a settings
    // receipt arrived: its reply shows the settings from before the change.
    QSet<QString> m_inspectionBehindSettings;
    QByteArray m_settingsIdentity; // The automatic apply in flight.
    bool m_pending = false, m_clipboard = false, m_rebuilding = false;
    // Polls keep hidden pages' data current; these pages render when shown.
    bool m_sessionsStale = false, m_dashboardStale = false;
    QSet<QString> m_retryReads;
    QHash<QString, quint64> m_waitingPreparations;
    quint64 m_waitingPreparationSerial = 0;
    bool m_draftRefreshPending = false;
    int m_tick = 0, m_activityPollMs = 2500, m_noticeMs = 5000;
    QTimer m_timer;
    QTimer m_sessionLoadingTimer;
    struct SessionRead { QString host, name, archive, agent; QElapsedTimer age; };
    QHash<QString, SessionRead> m_sessionReads;
    QHash<QString, QElapsedTimer> m_remotePollAges;
    BusyIndicator *m_sessionLoading = nullptr;
    QElapsedTimer m_processPollAge,m_inspectionAge;
    SessionList *m_sessions;
    SessionSearch *m_searchResults;
    QStackedWidget *m_listStack;
    MachinesPage *m_machinesPage;
    MachineFilter *m_machineFilter;
    ProjectFilter *m_projectFilter;
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
    QLabel *m_sessionFeedback = nullptr;
    QPushButton *m_noticeDismiss;
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
    // The machine of an untracked session's transfer, empty for a tracked one.
    QString m_terminalDropUntracked;
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
    QHash<QString, QJsonObject> m_pendingSendEvidence;
    QHash<QString, int> m_pendingEvidenceUsers;
    void beginSendEvidence(const QString &key, const QJsonObject &data);
    void finishSendEvidence(const QString &key);
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
