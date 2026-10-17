#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QToolButton>
#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QDir>
#include <QClipboard>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QProgressBar>
#include <QSaveFile>
#include <QSemaphore>
#include <atomic>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QStackedWidget>
#include <QSplitter>
#include <QScrollBar>
#include <QListWidget>
#include "GoToDialog.h"
#include "ShortcutsDialog.h"
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardItemModel>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <memory>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextTable>
#include <QTabWidget>
#include <QTabBar>
#include <QSlider>
#include <QFontInfo>
#include <QVBoxLayout>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTableWidget>
#include <QWindow>
#include <utility>
#include "SessionsWindow.h"
#include "ProcessView.h"
#include "ActivityView.h"
#include "AttachmentViewer.h"
#include "ExternalFile.h"
#include "QuestionCard.h"
#include "RecoveryWidgets.h"
#include "SettingsPage.h"
#include "WindowLayer.h"
#include "SwarmDialog.h"
#include "SwarmController.h"
#include "TerminalView.h"
#include "TerminalScreen.h"
#include "NewSessionDialog.h"
#include "DirectoryDialog.h"
#include "ProjectsDialog.h"
#include "MessageComposer.h"
#include "NativeBrowser.h"
#include "NativeBrowserPlugin.h"
#include "MessageBoxIcons.h"
#include "ComposerToolbar.h"
#include "SessionList.h"
#include "SessionCardDelegate.h"
#include "SessionOrganization.h"
#include "MachineFilter.h"
#include "ProjectFilter.h"
#include "SessionSearch.h"
#include "DashboardPage.h"
#include "AccountUsage.h"
#include "WorktreePanel.h"
#include "AccountUsageStore.h"
#include "MachinesPage.h"
#include "WorkspaceColorDialog.h"
#include "PopupMouse.h"
#include "AccountsPage.h"
#include "FakeCli.h"
#include "LaunchWarnings.h"
#include "LocalSessionHost.h"
#include "ProcessRunner.h"
#include "ProcessSettings.h"
#include "QuestionNotificationSettings.h"
#include "Shortcuts.h"

class FolderUrlRecorder : public QObject {
    Q_OBJECT
public:
    // Files that run programs are revealed instead; no test starts a file manager.
    FolderUrlRecorder() {
        QDesktopServices::setUrlHandler("file",this,"record");
        ExternalFile::setRevealHandler([this](const QString &path) { revealed<<path; return true; });
    }
    ~FolderUrlRecorder() override { QDesktopServices::unsetUrlHandler("file"); ExternalFile::setRevealHandler({}); }
    QList<QUrl> urls;
    QStringList revealed;
public slots:
    void record(const QUrl &url) { urls<<url; }
};

// Calls sample right after each step of the session panel animation, before
// layout requests deferred during that step run: what a repaint then would show.
class ExpansionSteps : public QObject {
public:
    explicit ExpansionSteps(std::function<void()> sample) : m_sample(std::move(sample)) {}
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::DynamicPropertyChange && static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "expansion")
            QMetaObject::invokeMethod(this, m_sample, Qt::QueuedConnection);
        return false;
    }
private:
    std::function<void()> m_sample;
};
// Question prompts as shown: the agent's Markdown, or plain text for terminal panels.
QStringList questionPrompts(const QuestionCard *card)
{
    QStringList prompts;
    for (const auto *prompt : card->findChildren<QWidget *>("questionPrompt")) {
        if (const auto *markdown = qobject_cast<const QTextBrowser *>(prompt)) prompts << markdown->toPlainText();
        else if (const auto *label = qobject_cast<const QLabel *>(prompt)) prompts << label->text();
    }
    return prompts;
}

// Presentation and persistence now complete asynchronously. Wait only for the
// exact selected draft before tests enter new text or inspect restored text.
static void setComposerText(MessageComposer *composer, const QString &text)
{
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    composer->editor()->setPlainText(text);
}

// Wait for a read accepted through the window's ordinary Refresh path. Earlier
// fake-CLI replies cannot then overwrite the next manually injected transition;
// the window's polling age is refreshed too, as it is for a real user refresh.
static void settleSelectedInspection(MessageComposer *composer, HgsClient *client,
                                    const QString &host, const QString &name, const QJsonObject &details)
{
    QSignalSpy loading(client, &HgsClient::inspectionLoadingChanged);
    QSignalSpy ready(client, &HgsClient::inspectionReady);
    qsizetype accepted = -1;
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        if (accepted >= 0) return true;
        const auto before = loading.size();
        composer->sessionRefreshRequested(composer->sessionKey());
        for (qsizetype i = before; i < loading.size(); ++i) {
            const auto &row = loading[i];
            if (row[0].toString() == host && row[1].toString() == name && row[2].toString().isEmpty()
                && row[3].toString().isEmpty() && row[4].toBool()) {
                accepted = ready.size(); return true;
            }
        }
        return false;
    })(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(([&] {
        for (qsizetype i = accepted; i < ready.size(); ++i) {
            const auto &row = ready[i];
            if (row[0].toString() == host && row[1].toString() == name && row[3].toString().isEmpty()
                && row[2].toJsonObject() == details) return true;
        }
        return false;
    })(), 10000);
}

// The window also polls the selected session. A fixture inspection replies with
// what the fixture said when it started, so one still running when a test injects
// details lands afterwards and replaces them. On Windows it can even arrive before a
// confirmation accepted from a short timer returns: Qt posts such timer events, and
// they share a batch with queued process replies (zerus-a0ak, zerus-a857). Call
// this before show(): the window then inspects only when something asks it to, such
// as show, selection, Refresh, a tab change or a changed conversation in a reply.
// HgsClient runs one inspection per session, so once the reply to the test's own
// inspection is in (settleSelectedInspection), none is pending and no poll follows.
static void keepDetailsPollsOut(SessionsWindow &window)
{
    window.setDetailsPollForTests(500, 24 * 60 * 60 * 1000);
}

class TestSessionsWindow : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void nativeSettingsNavigationOnExplicitSpeechHost();
    void copySessionReferencePreservesSelectionAndDraft();
    void sessionIdentifiersRemainInDetailsAcrossNavigation();
    void updatesEntryOpensExactSettingsPage();
    void fileCacheSettingsNavigation();
    void colorPickerUsesWorkspaceThemeAndCancelsOutside_data();
    void colorPickerUsesWorkspaceThemeAndCancelsOutside();
    void automaticApprovalSettingsNavigation_data();
    void automaticApprovalSettingsNavigation();
    void mobileConnectionSettingsOfferTheSharedDeliveryChoice();
    void controlOnlyComputerOffersOnlyRemoteMachines();
    void enablableConsoleNamesBothWaysToStartSessions();
    void controlOnlyComputerKeepsRecoveryOnRemoteMachines();
    void controlOnlyComputerAloneExplainsRecovery();
    void enabledWindowsComputerIsNoMobileGateway();
    void localWindowsSessionExplainsDisabledVoice();
    // Shards run functions and data rows in any order, so every test starts without
    // the settings an earlier one saved, window geometry included.
    void init() { ComposerDraftStore::flushWrites(); QSettings().clear(); QSettings().setValue("downloads/folder", m_dir->filePath("download-cache")); QVERIFY(QDir(ComposerDraftStore::directory()).removeRecursively()); }
    // Chords a test rebinds end with it.
    void cleanup() { ShortcutRegistry::instance().resetAll(); }
    void workspaceRestoresDraftAcrossRestartAndSessionRemoval();
    void savedDraftsGoToTheirSession_data();
    void savedDraftsGoToTheirSession();
    void independentWaitRemainsVisibleWithoutExpandingHeader_data();
    void independentWaitRemainsVisibleWithoutExpandingHeader();
    void gitPublicationChecksKeepDraftFocusAndSharedCards();
    void groupsPersistFilterAndRevealAttention();
    void emptyProjectsSettingPreservesArchiveAndProjects();
    void contentScaleLeavesWorkspaceChrome();
    void activityReadingColumnFollowsSettingsAndEdges();
    void activitySurfaceRemainsOpaque_data();
    void activitySurfaceRemainsOpaque();
    void settingsSlidersResetToDefaults();
    void projectSwarmFollowsWorkspaceTheme();
    void alwaysOnTopSettingKeepsWindowAbove();
    void railPinTogglesAlwaysOnTop();
    void windowLayerControlsKeepFocusAndLayout_data();
    void windowLayerControlsKeepFocusAndLayout();
    void unreadRepliesNeedAnActiveVisibleResult();
    void markAllReadIgnoresFiltersAndKeepsCurrentDraft();
    void sharedReadsQueueOnTheSessionHost();
    void unsentDraftBecomesARowStatus();
    void compactWorkspaceGeometry();
    void worktreeFilterKeepsConversationDraftAndSearchScope();
    void worktreePreview();
    void workingCardsKeepTurnClockAcrossUpdates();
    void questionReplyPreviewUsesCompletePrompt();
    void accountUsageRejectsOtherSessionReplies();
    void usageIndicatorSettingUpdatesFooter();
    void usageLimitBecomesToolbarChip();
    void earlyQuotaResetExplainsPendingNativeChoiceAndKeepsDraft();
    void openingChildClearsHiddenAccountUsageRecoveryNotice();
    void dashboardAndMultiMachineNavigation();
    void machineChooserCountsFollowStateScopeWithoutMachineOrTextFilters();
    void projectFilterCombinesScopesCountsAndNavigation();
    void pagedArchiveSearchJoinsExactProjectBeforeFiltering();
    void projectHeaderFiltersOnlyThisProject();
    void retiredGraphHasNoNavigationOrContextActions();
    void hiddenPagesRenderWhenShown();
    void pollsKeepUnchangedActivity();
    void multiSelectionKeepsConversationAndGroupMenu_data();
    void multiSelectionKeepsConversationAndGroupMenu();
    void selectionCommandsKeepPinnedTargets();
    void clearArchiveKeepsLiveAndNewEntries();
    void projectFolderActionsKeepTargets();
    void projectFolderNewSessionPrefillsExactTarget();
    void localWindowsLaunchJoinsTheFolderInAnyCase();
    void localWindowsFolderFilterIgnoresCase();
    void dashboardActivityRowsStayReadable_data();
    void dashboardActivityRowsStayReadable();
    void filtersSelectionAndOffline();
    void listFiltersKeepTheOpenConversation_data();
    void listFiltersKeepTheOpenConversation();
    void activeFilterCombinesWorkAndAttentionOnce();
    void backgroundShellRowKeepsActiveAndOpensItsProcesses();
    void activeFilterDraftsRestoreOfflineAndFollowEditsAndDelivery();
    void connectionStatusSurvivesNoticesAndRetries();
    void peerNamedDifferentlyListsSessionsUnderItsAlias();
    void refreshErrorsStayInComposerAndFooterWithoutMovingViews();
    void remoteSessionLoadingIsDelayedAndFollowsSelection();
    void backgroundPollKeepsActivityAndComposerStable();
    void processPollingFollowsVisibilityAndSettings();
    void recoveryCountdownKeepsHistoryDraftAndFocus();
    void recoveryEndingReturnsKeyboardFocusToField();
    void recoverySettingsValidateAndSaveForSelectedMachine();
    void recoverySettingsValidateAndSaveForSelectedMachine_data();
    void sessionLimitRecoveryShowsResetAndKeepsDraft();
    void sharedRecoverySyncCatchesUpOfflinePeer();
    void durableSubmissionKeepsGuiResponsive_data();
    void durableSubmissionKeepsGuiResponsive();
    void coldCacheConfirmationPreservesDraftAndPinsConversation();
    void nativeColdCacheChoiceKeepsExactAnswer_data();
    void nativeColdCacheChoiceKeepsExactAnswer();
    void coldCacheClearKeepsDraftAndPinsIdentity();
    void coldCacheExplainsUnavailableContextActions();
    void stoppedSessionStartsFreshOrSendsAfterStart();
    void localStartsAskAboutAnotherWindowsSession();
    void clearInspectionUpdatesActivityAndCountersImmediately();
    void coldCacheCompactWaitsForSuccess_data();
    void coldCacheCompactWaitsForSuccess();
    void composerNavigationDuringPolling();
    void attentionNavigationResetsFiltersAndTab();
    void sessionContextMenuSignalsAndKeyboard();
    void sessionAttentionMenuPersistsWithoutSelection();
    void sessionAttentionMenuSurvivesRoutinePoll();
    void activityMarkReadKeepsReadingPosition_data();
    void activityMarkReadKeepsReadingPosition();
    void fileDropsAcrossSessionPanelKeepDraftAndTarget();
    void terminalDropKeepsActivityDraftAndDoesNotSubmit();
    void sessionContextMenuGuardsIdentityAndChanges();
    void sessionContextMenuSavedAndArchive();
    void compactMetadataAndSubagentRoster();
    void nativeSessionUsageStaysWithSelectedConversation();
    void subagentActivityKeepsMainDraftAndRejectsStaleHistory();
    void childTreeKeepsRoutineResultsQuiet();
    void completedSubagentsPreferenceKeepsOpenHistory();
    void inactiveSubagentsStayInInspectorAndOutOfSessionCounts();
    void openCodeChildrenStayReadOnlyWithUnknownCounts();
    void sessionsRailResetsFiltersAndShowsAttention();
    void railButtonsIgnoreNativeLayoutMargins();
    void sessionRowsHaveNoHoverPopup();
    void subagentGroupsAndUnavailableActivity();
    void inspectorPreservesWorkspaceAndReportsTasks();
    void detailsPollingPreservesReadingPosition();
    void nativeGoalIsIndependentOfTurnState();
    void fileReferencesKeepMachineAndWorkspace();
    void activityAttachmentsOpenLikeDownloads();
    void fileReferenceOpeningOutlivesDialog_data();
    void fileReferenceOpeningOutlivesDialog();
    void terminalDropQuotesPathsForTheMachine();
    void terminalDropStagesIntoAnUntrackedRemoteShell();
    void folderShellFollowsTheMachinePlatform();
    void remoteFileDownloads_data();
    void remoteFileDownloads();
    void buttonFocusFollowsKeyboardAndNotClicks();
    void separatesConversationProcessAndTerminal();
    void activityEscapingAndActions();
    void messageDeliveryKeepsSessionIdentityAndDrafts_data();
    void messageDeliveryKeepsSessionIdentityAndDrafts();
    void providerErrorWaitsForNativePrompt_data();
    void providerErrorWaitsForNativePrompt();
    void quotaFailureExplainsRecoveryAndKeepsDraft();
    void firstMessageWaitsForNativePrompt_data();
    void firstMessageWaitsForNativePrompt();
    void resumedConversationPausesAndClearsOnceVerified();
    void failedMessagesReconcileAndRetryWithoutLosingDrafts();
    void modelSettingsKeepSessionIdentity_data();
    void modelSettingsKeepSessionIdentity();
    void scheduledSettingsApplyOncePerReady_data();
    void scheduledSettingsApplyOncePerReady();
    void coldQuestionSubmissionRevalidatesConsent_data();
    void coldQuestionSubmissionRevalidatesConsent();
    void questionAnswersStayWithOriginalSession();
    void queuedQuestionAnswersDisappearAfterSubmission();
    void terminalChoiceOpensOnlyTheRequestingTerminal_data();
    void terminalChoiceOpensOnlyTheRequestingTerminal();
    void optionalQuestionKeepsComposerAvailable();
    void optionalQuestionNotificationsAreOptIn();
    void questionCompletionKeepsInputFocus_data();
    void questionCompletionKeepsInputFocus();
    void questionFooterHasRoomAndKeepsAnswers_data();
    void questionFooterHasRoomAndKeepsAnswers();
    void startupTrustAnswerBeforeConversation_data();
    void startupTrustAnswerBeforeConversation();
    void archiveFiltersIdentityRestoreAndForget();
    void savedSessionsCanMoveToArchive();
    void archivingLastWorktreeSessionOffersCleanup();
    void terminatingWorktreeSessionOffersRemainingCleanupAndConsumesExactNativeOutcome();
    void nativeCleanupCanSupplyAnUncachedFolderAndArchiveTransitionsExpireOnlyTheirRepository();
    void renameValidationCancelAndSelection();
    void forkValidationCancelAndPinnedPayload();
    void forkKeepsSourceAndGroup_data();
    void forkKeepsSourceAndGroup();
    void forkDisabledForUnavailableSources();
    void renameOtherSessionKinds();
    void renameFailureKeepsSelection();
    void launchInitialFolderFollowsProject_data();
    void launchInitialFolderFollowsProject();
    void launchFolderFollowsSelectedSession_data();
    void launchFolderFollowsSelectedSession();
    void launchFolderPrefillPreservesUserChoices_data();
    void launchFolderPrefillPreservesUserChoices();
    void launchFolderPrefillRejectsUnverifiedMain_data();
    void launchFolderPrefillRejectsUnverifiedMain();
    void launchFoldersFollowProject();
    void launchListsSurviveFleetRefreshes();
    void sessionPanelHeaderStartsNewSession();
    void launchAddsOnlyFoldersOutsideTheProject();
    void automaticLaunchWorktree_data();
    void automaticLaunchWorktree();
    void arbitraryFolderAndTerminalPreference_data();
    void arbitraryFolderAndTerminalPreference();
    void terminationProgress_data();
    void terminationProgress();
    void listTerminationKeepsUnrelatedConversations_data();
    void listTerminationKeepsUnrelatedConversations();
    void remoteFolderLaunch();
    void launchNameFollowsRenameRules();
    void openCodeModelLaunchUsesNativeAccountAndActivity();
    void openCodeCapabilitiesPreserveDraftWithoutTerminal();
    void machineBatchCountsExcludeNativeOpenCodeSessions();
    void nativeLaunchStaysInWorkspace_data();
    void nativeLaunchStaysInWorkspace();
    void nativeSignInPreservesDraftAndUnblocks();
    void nativeUiOpensBrowserWithoutPlugin();
    void newSessionGroupFollowsLaunchIdentity_data();
    void newSessionGroupFollowsLaunchIdentity();
    void launchPlaceholderReportsASessionThatEndedUnlisted_data();
    void launchPlaceholderReportsASessionThatEndedUnlisted();
    void endedLaunchOutranksALaterStartedCallback_data();
    void endedLaunchOutranksALaterStartedCallback();
    void endedLaunchOutranksALaterFailure_data();
    void endedLaunchOutranksALaterFailure();
    void endedLaunchesInOneListingKeepUnshownFailures();
    void endedLaunchesWithoutAResultStayBounded();
    void folderBrowserIgnoresStaleResponses();
    void projectOrderAndDefaultBadge();
    void projectsEmbeddedAndRemote();
    void projectColorsApplyOrCancel();
    void legacyProjectsMigrateWithBackupAndLocalFolders();
    void batchActionsAndReturnToSessions();
    void searchResultKeepsItsSessionAndSnapshot();
    void blankListContextMenu();
    void contextMenuAcrossSessionPanelBackground();
    void sessionListCollapsesIntoWorkingStrip();
    void collapsedStripExpandsOverContentOnlyWhenEnabled();
    void collapsedStripSearchOpensPanel();
    void collapsedStripDoesNotOpenSubagents();
    void collapsingKeepsRowsInPlace();
    void escapeInActivityInterruptsAndRestoresPrompt();
    void stopNoticesExplainOriginalTargetAndCanBeDismissed_data();
    void stopNoticesExplainOriginalTargetAndCanBeDismissed();
    void escapeInterruptsFromAnywhereAndNeverCloses();
    void suggestedMessageIsPlaceholderAndTabTakesIt();
    // Keyboard commands; see docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md.
    void shortcutsSwitchSections();
    void shortcutsMoveBetweenSessions();
    void shortcutsFindSessionsNeedingAttention();
    void shortcutsCycleSessionTabs();
    void terminalChordReturnsToActivity();
    void shortcutsInModalDialogsStayLocal();
    void queueChordUsesTheExistingSendNowPath();
    void zoomChordsChangeContentScale();
    void reboundChordsApplyAtOnce();
    void projectListKeepsItsReorderKeys();
    void focusChordsCycleSessionAreas();
    void goToOpensAndActivatesItems();
    void cheatSheetOpensKeyboardSettings();
    void pushSendsNowWhenTheMessageIsQueued();
    void pushIgnoresQueuesWithoutTheMessage();
    void pushIgnoresTheMessageInsideAnotherWord();
    void pushDropsAfterSessionIdentityChange_data();
    void pushDropsAfterSessionIdentityChange();
    void pushFollowsTheRunThatTakesTheMessage();
    void pushStaysWithItsSession();
    void pushTimesOut();
    void pushOutlastedBySlowSend();
    void pushOfAttachmentsAloneSendsWithoutPush();
    void pushDropsWhenSendFails_data();
    void pushDropsWhenSendFails();
    void renameChordWorksOnTheSessionsPage();
    void attentionChordsStayInSearchResults();
    void goToFollowsTheSessionList();
    void pushBehindADialogIsNotSentAtTheDeadline();
    void pushReplacesAnEarlierPush();
    void pushWaitsForARunningSendNow();
    void pushIgnoresTheCodexQueueHeader();
    void pushFindsAMessageThatStartsWithAnAttachment_data();
    void pushFindsAMessageThatStartsWithAnAttachment();
    void preview();
    void controlConsolePreview();
private:
    FleetState fleet() const;
    // The base fixture's script; tests start program(), see FakeCli.h.
    QString script() const { return m_dir->filePath("hgs"); }
    QString program() const { return FakeCli::program(script()); }
    // A copy of the base fixture whose inspect answers with these details, so
    // polls that arrive while a test waits cannot replace them.
    QString programInspecting(const QString &name, const QJsonObject &details, const QJsonObject &child = {}, const QJsonObject &usage = {}) const;
    // Created by initTestCase(): listing the data rows (-datatags, as
    // RunQtShard.cmake does) exits without destroying the test object. That
    // listing calls every *_data() function first, so none may use m_dir.
    std::unique_ptr<QTemporaryDir> m_dir;
};

void TestSessionsWindow::nativeSettingsNavigationOnExplicitSpeechHost()
{
    const auto executable=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_HGS");
    const auto host=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_HOST");
    const auto expected=qEnvironmentVariable("ZERUS_SPEECH_SETTINGS_PROFILE");
    if(executable.isEmpty() || expected.isEmpty()) QSKIP("Requires an explicit installed CLI and speech profile; settings requests are read-only.");
    QSettings().setValue("speech/host",host); QSettings().setValue("speech/profile",expected);
    SettingsPage page(executable); page.setPeers({host}); page.show();
    auto *sections=page.findChild<QListWidget *>("settingsSections"); QVERIFY(sections);
    QCOMPARE(sections->item(sections->count()-1)->text(),"About");
    QCOMPARE(sections->item(sections->count()-2)->text(),"Keyboard");
    const int voice=sections->count()-3; QCOMPARE(sections->item(voice)->text(),"Voice input");
    sections->setCurrentRow(sections->count()-1);
    auto *version=page.findChild<QLabel *>("aboutHgsVersion"); QVERIFY(version);
    QTRY_VERIFY_WITH_TIMEOUT(version->text().startsWith("hgs CLI 1."),10000);
    auto *provider=page.findChild<QComboBox *>("speechProfile"); QVERIFY(provider);
    auto *refresh=page.findChild<QPushButton *>("speechRefresh"); QVERIFY(refresh);
    auto *status=page.findChild<QLabel *>("speechSettingsStatus"); QVERIFY(status);
    sections->setCurrentRow(voice);
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),65000);
    QVERIFY2(provider->isEnabled(),qPrintable(status->text())); QCOMPARE(provider->currentData().toString(),expected);
    page.hide(); page.show(); page.refresh();
    QVERIFY(!refresh->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(refresh->isEnabled(),65000);
    QVERIFY2(provider->isEnabled(),qPrintable(status->text())); QCOMPARE(provider->currentData().toString(),expected);
    sections->setCurrentRow(sections->count()-1);
    QTRY_VERIFY_WITH_TIMEOUT(version->text().startsWith("hgs CLI 1."),10000);
}

void TestSessionsWindow::initTestCase()
{
    MessageBoxIcons::install();
    m_dir = std::make_unique<QTemporaryDir>(); QVERIFY(m_dir->isValid());
    QCoreApplication::setOrganizationName("hgs-tests");
    QCoreApplication::setApplicationName("sessions-window");
    QGuiApplication::setDesktopFileName("org.example.ZerusSessionsWindowTest");
    QCoreApplication::setApplicationVersion(QLatin1String(HGS_TRAY_VERSION));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir->path());
    QVERIFY(FakeCli::install(script(), R"PY(#!/usr/bin/env python3
import json,sys
for value in sys.argv[1:]:print(value,file=sys.stderr)
a=sys.argv[1:]
if a and a[0].startswith('@'):a=a[1:]
c=a[0] if a else ''
if False:pass
elif c=='account' and a[1:2]==['ls']:
 print(json.dumps({'profiles':[{'id':'native-'+p,'provider':p,'label':'Default account','home':'/profiles/'+p,'native':True,'installed':True,'is_default':True}
                              for p in ['codex','claude','kimi','dsh','opencode']]}))
elif c=='inspect':print('{"tracked":true,"conversation_id":"conversation-one","events":[],"cursor":0}')
elif c=='project':
 if a[1:2]==['ls']:print('[{"name":"infra","dir":"/remote/infra","exists":true,"src":"ansible"}]')
elif c=='worktrees':sys.stdout.write(open(__file__+'.worktrees').read())
elif c=='dirs':print('{"path":"/remote/work tree","parent":"/remote","home":"/remote","directories":[{"name":"docs","path":"/remote/work tree/docs"}]}')
elif c=='ls':print('{"host":"mac","ok":true,"sessions":[]}')
)PY"));
    QFile catalogFile(script()+".worktrees");QVERIFY(catalogFile.open(QIODevice::WriteOnly));
    auto catalog=QJsonDocument::fromJson(R"({"state":"ok","path":"/repo","selected_root":"/repo","common_dir":"/repo/.git","worktrees":[{"path":"/repo","kind":"main","branch":"main","available":true},{"path":"/linked","kind":"linked","branch":"feat/worktree-catalog","available":true},{"path":"/empty","kind":"linked","branch":"fix/empty-copy","available":true}]})").object();catalog["sampled_at"]=QDateTime::currentSecsSinceEpoch();catalogFile.write(QJsonDocument(catalog).toJson());
}

QString TestSessionsWindow::programInspecting(const QString &name, const QJsonObject &details, const QJsonObject &child, const QJsonObject &usage) const
{
    QFile original(script()); if (!original.open(QIODevice::ReadOnly)) return {};
    QByteArray answer;
    if (!child.isEmpty()) answer = "elif c=='inspect' and '--agent' in a:print(" + FakeCli::pythonString(QString::fromUtf8(QJsonDocument(child).toJson(QJsonDocument::Compact))) + ")\n";
    answer += "elif c=='inspect':print(" + FakeCli::pythonString(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact))) + ")\n";
    if (!usage.isEmpty()) answer += "elif c=='account' and a[1:2]==['inspect']:print(" + FakeCli::pythonString(QString::fromUtf8(QJsonDocument(usage).toJson(QJsonDocument::Compact))) + ")\n";
    const QString fixture = m_dir->filePath(name);
    return FakeCli::install(fixture, original.readAll().replace("if False:pass\n", "if False:pass\n" + answer)) ? FakeCli::program(fixture) : QString();
}

FleetState TestSessionsWindow::fleet() const
{
    SessionInfo codex; codex.name = "codex/hgs/dashboard"; codex.cmd = "codex"; codex.project = "hgs"; codex.tag = "dashboard";
    codex.tracked = true; codex.resumable = true; codex.activity = "busy"; codex.phase = "tool";
    codex.currentTool = "Bash"; codex.toolDetail = "ctest --test-dir tray/build"; codex.model = "gpt-6.1-sol"; codex.effort = "high";
    codex.prompt = "Build a clear view of every agent across our machines.";
    auto kimi = codex; kimi.name = "kimi/docs/research"; kimi.cmd = "kimi"; kimi.project = "docs"; kimi.tag = "research";
    kimi.currentTool.clear(); kimi.activity = "idle"; kimi.phase = "idle"; kimi.prompt = "Compare the session lifecycle across our CLI agents.";
    auto saved = codex; saved.name = "codex/website/navigation"; saved.project = "website"; saved.tag = "navigation";
    saved.state = "paused"; saved.currentTool.clear(); saved.prompt = "Simplify the mobile navigation.";
    SessionInfo old; old.name = "sh/scratch"; old.cmd = "sh"; old.project = "scratch";
    BoxState arch; arch.host = "arch"; arch.ok = true; arch.sessions = {codex, kimi, saved, old};
    // Its folders are POSIX on every GUI; tests of local Windows paths set their own platform.
    arch.metrics = QJsonObject{{"platform", "linux"}};
    auto approval = codex; approval.name = "claude/infra/review"; approval.cmd = "claude"; approval.project = "infra";
    approval.tag = "review"; approval.phase = "approval"; approval.currentTool = "Bash";
    approval.toolDetail = "terraform plan"; approval.prompt = "Review infrastructure changes before the next deployment.";
    BoxState mac; mac.host = "mac"; mac.ok = true; mac.sessions = {approval};
    FleetState result; result.setLocal(arch, QDateTime::currentMSecsSinceEpoch()); result.setPeer(mac, QDateTime::currentMSecsSinceEpoch());
    return result;
}

void TestSessionsWindow::gitPublicationChecksKeepDraftFocusAndSharedCards()
{
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto program = fixture.filePath("hgs"); QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#!/usr/bin/env python3\nimport runpy,sys,time\nif sys.argv[1:2]==['git-status']:\n"
        " open(__file__+'.calls','a').write(' '.join(sys.argv[1:])+'\\n');time.sleep(0.4)\n"
        " print('{\"state\":\"ok\",\"root\":\"/repo\",\"branch\":\"main\",\"upstream\":\"origin/main\",\"changed_files\":4,\"ahead\":2,\"behind\":0,\"remote_state\":\"verified\",\"remote_age_seconds\":0}')\n"
        "else:\n sys.argv[0]=" + FakeCli::pythonString(script()) + ";runpy.run_path(sys.argv[0],run_name='__main__')\n"); file.close();
    QVERIFY(FakeCli::makeRunnable(file.fileName()));
    auto state = fleet(); auto box = state.local();
    for (int i = 0; i < 2; ++i) { box.sessions[i].cwd = "/repo"; box.sessions[i].gitRoot = "/repo"; box.sessions[i].gitBranch = "main"; box.sessions[i].gitMetadataState = "ok"; }
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(program)); window.resize(1120, 680); window.setFleet(state); window.show();
    window.showSession({}, box.sessions[0].name);
    auto *list = window.findChild<SessionList *>("sessionList"); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    QVERIFY(list); QVERIFY(composer); setComposerText(composer, "Keep this unsent draft");
    composer->editor()->setFocus(); window.activateWindow(); QTRY_VERIFY(composer->editor()->hasFocus());
    const auto currentKey = list->currentItem()->data(SessionRoles::Key).toString();
    auto count = [&] { int result = 0; for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(SessionRoles::GitStatus).toJsonObject().value("changed_files").toInt() == 4) ++result;
        return result; };
    QTRY_COMPARE(count(), 2);
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep this unsent draft")); QVERIFY(composer->editor()->hasFocus());
    QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(), currentKey);
    QFile calls(program + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll().count('\n'), 1);
    QVERIFY(list->currentItem()->data(Qt::AccessibleDescriptionRole).toString().contains("4 changed or new files"));
    const auto directory = qEnvironmentVariable("HGS_GIT_STATUS_PREVIEW");
    if (!directory.isEmpty()) { QDir().mkpath(directory); QVERIFY(window.grab().save(directory + "/integrated.png")); }
}

void TestSessionsWindow::workspaceRestoresDraftAcrossRestartAndSessionRemoval()
{
    const auto state = fleet();
    {
        SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, "codex/hgs/dashboard");
        auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
        setComposerText(composer, "Keep my work after the agent exits");
        QVERIFY(composer->addAttachment("notes.txt", "text/plain", "notes snapshot"));
    }
    SessionsWindow restored(program()); restored.setFleet(state); restored.show(); restored.showSession({}, "codex/hgs/dashboard");
    auto *composer = restored.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    QTRY_COMPARE(composer->editor()->toPlainText(), "[File #1] Keep my work after the agent exits");
    restored.setFleet(FleetState());
    QTimer::singleShot(0, &restored, [&] {
        auto *dialog = restored.findChild<QDialog *>("savedDraftsDialog"); QVERIFY(dialog); QVERIFY(dialog->isVisible());
        auto *list = dialog->findChild<QListWidget *>("savedDraftList"); QTRY_COMPARE(list->count(), 1);
        QCOMPARE(dialog->findChild<QPlainTextEdit *>("savedDraftPreview")->toPlainText(), "[File #1] Keep my work after the agent exits");
        QVERIFY(!dialog->findChild<QPushButton *>("restoreSavedDraft")->isEnabled()); dialog->reject();
    });
    restored.findChild<QPushButton *>("savedDrafts")->click();
}

void TestSessionsWindow::colorPickerUsesWorkspaceThemeAndCancelsOutside_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark")<<QString("dark");QTest::newRow("light")<<QString("light");
}
void TestSessionsWindow::colorPickerUsesWorkspaceThemeAndCancelsOutside()
{
    QFETCH(QString,theme);QSettings().setValue("workspace/theme",theme);
    SessionsWindow window(program());window.setFleet(fleet());window.resize(1100,760);window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
    WorkspaceColorDialog picker(QColor("#5c9abf"),&window);picker.setWindowTitle("Color (synthetic preview)");
    bool expired=false,handled=false;
    QTimer::singleShot(5000,&picker,[&]{expired=true;picker.reject();});
    QTimer::singleShot(0,&picker,[&]{
        QCOMPARE(picker.palette().color(QPalette::Window),QColor(theme=="dark"?"#161b21":"#f5f7f9"));
        auto *buttons=picker.findChild<QDialogButtonBox *>();QVERIFY(buttons);
        auto *cancel=buttons->button(QDialogButtonBox::Cancel);QVERIFY(cancel);
        QTRY_VERIFY(cancel->geometry().left()>buttons->button(QDialogButtonBox::Ok)->geometry().right());
        const auto previews=qEnvironmentVariable("HGS_COLOR_PICKER_PREVIEW");
        if(!previews.isEmpty()){QDir().mkpath(previews);QVERIFY(picker.grab().save(previews+"/color-"+theme+".png"));}
        const QPoint point(window.width()-8,window.height()-8);QVERIFY(!picker.frameGeometry().contains(window.mapToGlobal(point)));
        QVERIFY(PopupMouse::click(&window,Qt::LeftButton,point));
        QTRY_VERIFY(!picker.isVisible());handled=true;
    });
    QCOMPARE(picker.exec(),int(QDialog::Rejected));QVERIFY(handled);QVERIFY(!expired);QVERIFY(!picker.selectedColor().isValid());
}

void TestSessionsWindow::savedDraftsGoToTheirSession_data()
{
    QTest::addColumn<QString>("theme");
    QTest::addColumn<bool>("doubleClick");
    QTest::newRow("dark") << QString("dark") << false;
    QTest::newRow("light") << QString("light") << false;
    QTest::newRow("dark-double-click") << QString("dark") << true;
    QTest::newRow("light-double-click") << QString("light") << true;
}

void TestSessionsWindow::savedDraftsGoToTheirSession()
{
    QFETCH(QString, theme); QFETCH(bool, doubleClick); QSettings().setValue("workspace/theme", theme);
    auto state = fleet(); auto box = state.local();
    box.sessions[1].conversationId = "conversation-one"; box.sessions[1].subagentSource = "hooks";
    box.sessions[1].subagents = {{"worker", QJsonObject{{"name", "Worker"}, {"state", "working"}}}};
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    const QString remote = "mac\nclaude/infra/review", child = "arch\nkimi/docs/research\nconversation-one\nworker", ended = "mac\nclaude/infra/ended";
    ComposerDraftStore store;
    for (const auto &[key, text] : {std::pair{remote, QString("Check the plan first")}, std::pair{child, QString("Ask the worker")},
                                    std::pair{ended, QString("From an ended session")}}) {
        ComposerDraft draft; draft.text = text; QVERIFY(store.save(key, draft));
    }
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    auto *childComposer = window.findChild<MessageComposer *>("subagentComposer"); QVERIFY(childComposer);
    const auto choose = [&](const QString &key, bool available) {
        QTimer::singleShot(0, &window, [&, key, available] {
            auto *dialog = window.findChild<QDialog *>("savedDraftsDialog"); QVERIFY(dialog);
            auto *list = dialog->findChild<QListWidget *>("savedDraftList"); QTRY_COMPARE(list->count(), 3);
            QVERIFY(dynamic_cast<SessionDelegate *>(list->itemDelegate()));
            for (int i = 0; i < list->count(); ++i) {
                const auto *item = list->item(i);
                QVERIFY(!item->text().section('\n', 0, 0).trimmed().isEmpty());
                QVERIFY(list->visualItemRect(item).height() >= 2 * list->fontMetrics().height());
                QVERIFY(item->toolTip().isEmpty());QVERIFY(!item->text().contains(QChar(0x00b7)));
                QVERIFY(!item->data(SessionRoles::ModifiedAt).toString().isEmpty());
                if(item->data(Qt::UserRole).toString()==remote){
                    QCOMPARE(item->data(SessionRoles::Title).toString(),QString("review"));
                    QCOMPARE(item->data(SessionRoles::Agent).toString(),QString("claude"));
                    QCOMPARE(item->data(SessionRoles::Host).toString(),QString("mac"));
                    QCOMPARE(item->data(SessionRoles::Model).toString(),state.peer("mac")->sessions[0].model);
                }
            }
            list->setFocus(); list->setCurrentRow(0);
            QTest::keyClick(list, Qt::Key_Down); QCOMPARE(list->currentRow(), 1);
            QTest::keyClick(list, Qt::Key_Up); QCOMPARE(list->currentRow(), 0);
            for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(Qt::UserRole).toString() == key) list->setCurrentRow(i);
            QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), key);
            auto *open = dialog->findChild<QPushButton *>("openSavedDraftSession"); QCOMPARE(open->isEnabled(), available);
            const auto preview = qEnvironmentVariable("HGS_SAVED_DRAFTS_PREVIEW");
            if (key == ended && !preview.isEmpty()) {
                dialog->setWindowTitle("Saved drafts (synthetic preview)");
                QTRY_COMPARE(dialog->findChild<QPlainTextEdit *>("savedDraftPreview")->toPlainText(), QString("From an ended session"));
                list->scrollToItem(list->currentItem());
                QDir().mkpath(preview); QTest::qWait(50);
                QVERIFY(dialog->grab().save(preview + "/drafts-" + theme + ".png"));
                dialog->resize(620, 460); list->scrollToItem(list->currentItem()); QTest::qWait(50);
                QVERIFY(dialog->grab().save(preview + "/drafts-narrow-" + theme + ".png"));
            }
            if (doubleClick) {
                list->scrollToItem(list->currentItem());
                const auto point = list->visualItemRect(list->currentItem()).center();
                QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point);
                QTest::mouseDClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point);
                QCOMPARE(dialog->isVisible(), !available);
                if (!available) dialog->reject();
            } else if (available) open->click(); else dialog->reject();
        });
        window.findChild<QPushButton *>("savedDrafts")->click();
    };
    choose(ended, false); QCOMPARE(composer->sessionKey(), QString("\ncodex/hgs/dashboard"));
    choose(remote, true); QCOMPARE(composer->sessionKey(), remote);
    QTRY_COMPARE(composer->editor()->toPlainText(), QString("Check the plan first"));
    // A subagent key names this computer; Go to opens the subagent in its session.
    choose(child, true); QCOMPARE(composer->sessionKey(), QString("\nkimi/docs/research"));
    QCOMPARE(childComposer->sessionKey(), child); QTRY_COMPARE(childComposer->editor()->toPlainText(), QString("Ask the worker"));
    QCOMPARE(window.findChild<SessionList *>("sessionList")->currentItem()->data(SessionRoles::ChildId).toString(), QString("worker"));
    choose(remote, true); QCOMPARE(composer->sessionKey(), remote);
    QVERIFY(window.findChild<SessionList *>("sessionList")->currentItem()->data(SessionRoles::ChildId).toString().isEmpty());
}

void TestSessionsWindow::independentWaitRemainsVisibleWithoutExpandingHeader_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark")<<QString("dark"); QTest::newRow("light")<<QString("light");
}

void TestSessionsWindow::independentWaitRemainsVisibleWithoutExpandingHeader()
{
    QFETCH(QString,theme); QSettings().setValue("workspace/theme",theme);
    auto state=fleet(); auto box=state.local(); auto &s=box.sessions[0];
    s.tracked=true; s.activity="idle"; s.phase="idle"; s.runId="waiting-run";s.conversationId="waiting-conversation";
    s.backgroundProcesses=0;s.subagentActiveCount=0;s.subagentCount=0;
    s.coordinationWait=QJsonObject{{"schema",1},{"status","ok"},{"revision",1},{"run_id",s.runId},{"conversation_id",s.conversationId},{"count",1},
        {"items",QJsonArray{QJsonObject{{"state","awaiting_result"},{"created_at",QDateTime::currentSecsSinceEpoch()-300},
            {"producer",QJsonObject{{"name","claude/project/worker-"+QString(100,'w')},{"host","peer"}}},{"label","Review the patch"}}}}};
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.resize(1000,800);window.setFleet(state);window.show();window.showSession({},s.name);
    auto *list=window.findChild<SessionList *>("sessionList");QVERIFY(list->currentItem());
    QVERIFY(list->currentItem()->data(SessionRoles::Waiting).toBool());
    QVERIFY(list->currentItem()->data(SessionRoles::Detail).toString().contains("Review the patch"));
    auto *badge=window.findChild<QLabel *>("badge");QVERIFY(badge);
    QTRY_VERIFY(badge->text().startsWith("Waiting for result ("));
    QVERIFY(!badge->text().contains("worker"));
    QVERIFY(window.width()<=1100);QVERIFY(badge->width()<=300);
    const auto preview=qEnvironmentVariable("ZERUS_WAIT_QA_DIR");
    if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/wait-"+theme+".png"));}
    s.coordinationWait["revision"]=2;s.coordinationWait["count"]=0;s.coordinationWait["items"]=QJsonArray{};
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    QVERIFY(!list->currentItem()->data(SessionRoles::Waiting).toBool());
}

void TestSessionsWindow::workingCardsKeepTurnClockAcrossUpdates()
{
    auto state=fleet(); auto box=state.local();
    const double started=QDateTime::currentSecsSinceEpoch()-1076;
    box.sessions[0].turnStarted=started; state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({},"codex/hgs/dashboard");
    auto *list=window.findChild<SessionList *>("sessionList"); auto *row=list->currentItem(); QVERIFY(row);
    QCOMPARE(row->data(SessionRoles::WorkingSince).toDouble(),started);
    QVERIFY(row->data(SessionRoles::Working).toBool()); QVERIFY(list->elapsedTimerRunning());
    list->setActivityAnimationEnabled(false); QVERIFY(list->elapsedTimerRunning());
    QSignalSpy reset(list->model(),&QAbstractItemModel::modelReset);
    QTest::qWait(1100); QCOMPARE(list->currentItem(),row); QCOMPARE(reset.size(),0);
    QCOMPARE(row->data(SessionRoles::WorkingSince).toDouble(),started);
    box.sessions[0].phase="compacting"; box.sessions[0].lastEventAt=QDateTime::currentSecsSinceEpoch();
    const double compactStarted=QDateTime::currentSecsSinceEpoch()-83;
    box.sessions[0].compactionStarted=compactStarted;
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->currentItem(),row); QCOMPARE(row->data(SessionRoles::WorkingSince).toDouble(),compactStarted);
    QCOMPARE(row->data(SessionRoles::Status).toString(),QString("Compacting"));
    const auto preview=qEnvironmentVariable("HGS_ELAPSED_PREVIEW");
    if(!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(list->grab().save(preview+"/working-timer.png")); }
    box.sessions[0].phase="working";
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(row->data(SessionRoles::WorkingSince).toDouble(),started);
    box.sessions[0].activity="idle"; box.sessions[0].phase="idle";
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(!row->data(SessionRoles::Working).toBool()); QVERIFY(!list->elapsedTimerRunning());
}

void TestSessionsWindow::questionReplyPreviewUsesCompletePrompt()
{
    auto state=fleet();auto box=state.local();auto &session=box.sessions[0];
    const QString answer="Review the desktop changes on both machines.";
    const QJsonObject reply{{"questionItemId","reply-one"},{"question",QString(300,'q')},{"answer",answer}};
    session.prompt="<send_user_message_question_reply>\n"+QString::fromUtf8(QJsonDocument(QJsonArray{reply}).toJson(QJsonDocument::Compact))
        +"\n</send_user_message_question_reply>";
    session.activitySummary="Working";session.activityDetail=session.prompt.left(240)+QChar(0x2026);
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.setFleet(state);window.show();window.showSession({},session.name);
    auto *list=window.findChild<SessionList *>("sessionList");auto *row=list->currentItem();QVERIFY(row);
    QCOMPARE(row->data(SessionRoles::Detail).toString(),"Working: Your answer: "+answer);
    QVERIFY(row->data(Qt::AccessibleTextRole).toString().contains(answer));
    QVERIFY(!row->data(Qt::AccessibleTextRole).toString().contains("send_user_message_question_reply"));
    QCOMPARE(session.activityDetail,session.prompt.left(240)+QChar(0x2026));
}

void TestSessionsWindow::machineChooserCountsFollowStateScopeWithoutMachineOrTextFilters()
{
    auto state=fleet(); auto local=state.local();
    auto archive=local.sessions[0]; archive.state="archived"; archive.archiveId="archive-one";
    local.sessions.append(archive); state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    BoxState empty; empty.host="empty"; empty.ok=true; state.setPeer(empty,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); keepDetailsPollsOut(window); window.setFleet(state); window.show();
    auto *filter=window.findChild<MachineFilter *>("machineFilterChips"); auto *menu=filter->button()->menu();
    menu->popup(window.mapToGlobal(QPoint(100,100))); QTRY_VERIFY(menu->isVisible());
    const auto count=[&](const QString &host) {
        for (auto *choice : menu->findChildren<QCheckBox *>("machineFilterChoice"))
            if (choice->property("host")==host) return choice->property("sessionCount").toInt();
        return -1;
    };
    QCOMPARE(count("@local"),4); QCOMPARE(count("mac"),1); QCOMPARE(count("empty"),0);
    filter->setSelection({"@local"}); QCOMPARE(count("mac"),1); // Unselected machines retain their counts.
    menu->hide();
    window.findChild<DashboardPage *>()->filterRequested("@all","paused");
    menu->popup(window.mapToGlobal(QPoint(100,100))); QCOMPARE(count("@local"),1); QCOMPARE(count("mac"),0);
    menu->hide(); window.findChild<DashboardPage *>()->filterRequested("@all","archived");
    menu->popup(window.mapToGlobal(QPoint(100,100))); QCOMPARE(count("@local"),1); QCOMPARE(count("mac"),0);
    menu->hide(); window.findChild<DashboardPage *>()->filterRequested("@all","all");
    window.findChild<QLineEdit *>("search")->setText("does-not-match-any-session");
    menu->popup(window.mapToGlobal(QPoint(100,100))); QCOMPARE(count("@local"),4); QCOMPARE(count("mac"),1);
    local.sessions.removeFirst(); state.setLocal(local,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(count("@local"),3); QCOMPARE(count("mac"),1); QVERIFY(menu->isVisible()); menu->hide();
}

void TestSessionsWindow::retiredGraphHasNoNavigationOrContextActions()
{
    SessionsWindow window(script()); keepDetailsPollsOut(window);
    window.setFleet(fleet()); window.resize(1100,760); window.show();
    const auto name=fleet().local().sessions[0].name; window.showSession({},name);
    auto *list=window.findChild<SessionList *>("sessionList"); QVERIFY(list && list->currentItem());
    auto *composer=window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    setComposerText(composer,"Keep the current conversation draft");
    for(auto *button:window.findChildren<QPushButton *>("railButton"))
        QVERIFY(button->property("glyph")!="graph");
    auto *dashboard=window.findChild<DashboardPage *>(); QVERIFY(dashboard);
    auto *pages=qobject_cast<QStackedWidget *>(dashboard->parentWidget()); QVERIFY(pages);
    QCOMPARE(pages->count(),6);
    list->customContextMenuRequested(list->visualItemRect(list->currentItem()).center());
    auto *menu=window.findChild<QMenu *>("sessionContextMenu"); QVERIFY(menu);
    QVERIFY(!menu->findChild<QAction *>("contextShowInGraph"));
    QVERIFY(menu->findChild<QAction *>("contextCopyCommand")); menu->close();
    auto *projects=window.findChild<ProjectsDialog *>(); QVERIFY(projects);
    for(auto *button:window.findChildren<QPushButton *>("railButton"))
        if(button->property("glyph")=="projects") button->click();
    QTRY_VERIFY(projects->isVisible());
    window.showSession({},name);
    QTRY_COMPARE(composer->editor()->toPlainText(),QString("Keep the current conversation draft"));
}

void TestSessionsWindow::pagedArchiveSearchJoinsExactProjectBeforeFiltering()
{
    const QString name="codex/hgs/dashboard", a="10000000-0000-4000-8000-000000000001",b="10000000-0000-4000-8000-000000000002";
    SessionOrganization org;const auto first=org.createGroup("Focus"),second=org.createGroup("Other");
    org.moveSession(org.observe("arch",name,"archive-run",a),first);org.moveSession(org.observe("arch",name,"archive-run",b),second);
    QSettings().setValue("workspace/organization",QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    QTemporaryDir fixture;const auto cli=fixture.filePath("hgs");
    QVERIFY(FakeCli::install(cli,R"PY(#!/usr/bin/env python3
import json,sys
args=sys.argv[1:]
remote=bool(args and args[0].startswith('@'))
if remote:args=args[1:]
if args[0]=='search':
 rows=[]
 if not remote:
  for n in (1,2):
   ident='10000000-0000-4000-8000-00000000000'+str(n)
   session={'name':'codex/hgs/dashboard','cmd':'codex','run_id':'archive-run','conversation_id':'archived-conversation','state':'archived','archive_id':ident,'archived_at':n,'tracked':True,'resumable':True}
   rows.append(dict(name=session['name'],run_id=session['run_id'],conversation_id=session['conversation_id'],archive_id=ident,session=session,message_id='message'+str(n),at=n,role='assistant',snippet='Needle archived '+str(n),event={'detail':'Needle archived '+str(n)}))
 print(json.dumps({'query':'needle','results':rows,'truncated':False}))
elif args[0]=='inspect':print(json.dumps({'name':args[1],'run_id':'archive-run','conversation_id':'archived-conversation','archive_id':args[args.index('--archive')+1] if '--archive' in args else '', 'tracked':True,'state':'archived','events':[],'cursor':0}))
else:print('{}')
)PY"));
    auto state=fleet();auto local=state.local();local.archivePaging=true;local.sessions[0].runId="live-run";local.sessions[0].prompt.clear();state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(cli));keepDetailsPollsOut(window);window.setFleet(state);window.resize(1100,760);window.show();
    auto *projects=window.findChild<ProjectFilter *>("projectFilterChips");QVERIFY(projects);projects->setSelection({first});
    auto *query=window.findChild<QLineEdit*>("search");auto *results=window.findChild<QListWidget*>("searchResults");QVERIFY(query&&results);query->setText("needle");
    QTRY_COMPARE(results->count(),1);QVERIFY(SessionSearch::sessionKey(results->item(0)).endsWith(a));
    for(auto *filter:window.findChildren<QPushButton*>("sessionFilter"))if(filter->property("filter")=="archived"){QCOMPARE(filter->property("count").toInt(),1);QVERIFY(filter->text().startsWith("1"));}
    projects->setSelection({second});QTRY_COMPARE(results->count(),1);QVERIFY(SessionSearch::sessionKey(results->item(0)).endsWith(b));
    projects->setSelection({});QTRY_COMPARE(results->count(),2);
}

void TestSessionsWindow::projectFilterCombinesScopesCountsAndNavigation()
{
    SessionOrganization org;
    const auto work = org.createGroup("Focus"), saved = org.createGroup("Focus"); // Names never identify the scope.
    org.moveSession(org.observe("arch", "codex/hgs/dashboard", ""), work);
    org.moveSession(org.observe("mac", "claude/infra/review", ""), work);
    org.moveSession(org.observe("arch", "codex/website/navigation", ""), saved);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); keepDetailsPollsOut(window); window.setFleet(fleet()); window.resize(1100,760); window.show();
    auto *projects = window.findChild<ProjectFilter *>("projectFilterChips"); auto *machines = window.findChild<MachineFilter *>("machineFilterChips");
    auto *list = window.findChild<SessionList *>("sessionList");
    const auto parents = [&] { int count=0; for(int i=0;i<list->count();++i) if(!list->item(i)->data(SessionRoles::Header).toBool()) ++count; return count; };
    projects->setSelection({work}); QCOMPARE(parents(),2);
    for (auto *button : window.findChildren<QPushButton *>("sessionFilter")) if (button->property("filter")=="all") QCOMPARE(button->property("count").toInt(),2);
    projects->setSelection({work,saved}); QCOMPARE(parents(),3);
    auto *menu = projects->button()->menu(); menu->popup(window.mapToGlobal(QPoint(100,100))); QTRY_VERIFY(menu->isVisible());
    const auto choice = [&](const QString &id) -> QCheckBox * {
        for (auto *check : menu->findChildren<QCheckBox *>("projectFilterChoice")) if (check->property("project")==id) return check;
        return nullptr;
    };
    auto *workChoice=choice(work), *savedChoice=choice(saved); QVERIFY(workChoice); QVERIFY(savedChoice);
    QCOMPARE(workChoice->property("sessionCount").toInt(),2); QCOMPARE(savedChoice->property("sessionCount").toInt(),1);
    workChoice->click(); QCOMPARE(projects->selection(),QSet<QString>{saved}); QVERIFY(menu->isVisible()); workChoice->click();
    machines->setSelection({"mac"}); QCOMPARE(parents(),1);
    QCOMPARE(choice(work),workChoice); QCOMPARE(choice(saved),savedChoice); QVERIFY(menu->isVisible());
    QCOMPARE(workChoice->property("sessionCount").toInt(),1); QCOMPARE(savedChoice->property("sessionCount").toInt(),0);
    menu->hide(); machines->setSelection({});
    // Search uses the exact logical membership for both metadata and content hits.
    projects->setSelection({work}); auto *search=window.findChild<QLineEdit *>("search"); search->setText("navigation");
    auto *results=window.findChild<QListWidget *>("searchResults"); QCOMPARE(results->count(),0);
    projects->setSelection({saved}); QCOMPARE(results->count(),1); search->clear();
    window.showProjects(); window.showSessionList();
    QCOMPARE(projects->selection(),QSet<QString>{saved}); QCOMPARE(parents(),1);
    for (auto *button : window.findChildren<QPushButton *>("sessionFilter")) if (button->property("filter")=="active") button->click();
    QCOMPARE(parents(),0); // The selected project has a saved session only.
    for (auto *button : window.findChildren<QPushButton *>("sessionFilter")) if (button->property("filter")=="all") button->click();
    QCOMPARE(projects->selection(),QSet<QString>{saved}); QCOMPARE(parents(),1);
    // A catalog refresh, session rename and explicit all-current-projects selection stay exact.
    auto state=fleet(); auto local=state.local(); local.sessions[2].runId="saved-run";
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    local.sessions[2].name="codex/website/renamed"; state.setLocal(local,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(parents(),1); QCOMPARE(projects->selection(),QSet<QString>{saved});
    const auto preview=qEnvironmentVariable("HGS_PROJECT_FILTER_PREVIEW");
    if(!preview.isEmpty()) {
        QDir().mkpath(preview); projects->setSelection({work,saved});
        for(const auto &theme:{QStringLiteral("dark"),QStringLiteral("light")}) {
            auto *themeChoice=window.findChild<QComboBox *>("workspaceTheme"); QVERIFY(themeChoice);
            themeChoice->setCurrentIndex(themeChoice->findData(theme));
            QTest::qWait(100); QVERIFY(window.grab().save(preview+"/projects-"+theme+".png"));
            menu->popup(window.mapToGlobal(QPoint(110,180))); QTest::qWait(50); QVERIFY(menu->grab().save(preview+"/chooser-"+theme+".png")); menu->hide();
        }
    }
    window.showSession("mac","claude/infra/review"); QVERIFY(projects->selection().isEmpty()); QCOMPARE(parents(),5);
}

void TestSessionsWindow::projectHeaderFiltersOnlyThisProject()
{
    SessionOrganization org; const auto work=org.createGroup("Work"), docs=org.createGroup("Docs");
    org.moveSession(org.observe("arch","codex/hgs/dashboard",""),work); org.moveSession(org.observe("mac","claude/infra/review",""),docs);
    QSettings().setValue("workspace/organization",QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); keepDetailsPollsOut(window); window.setFleet(fleet()); window.show();
    auto *list=window.findChild<SessionList *>("sessionList"); auto *projects=window.findChild<ProjectFilter *>("projectFilterChips");
    const auto openMenu=[&](const QString &id) -> QMenu * {
        for(int i=0;i<list->count();++i) if(list->item(i)->data(SessionRoles::Header).toBool()&&list->item(i)->data(SessionRoles::Group)==id) {
            list->scrollToItem(list->item(i)); list->customContextMenuRequested(list->visualItemRect(list->item(i)).center());
            return window.findChild<QMenu *>("sessionGroupMenu");
        }
        return nullptr;
    };
    const auto action=[](QMenu *menu,const QString &name) -> QAction * { for(auto *a:menu->actions()) if(a->objectName()==name)return a;return nullptr; };
    auto *menu=openMenu(work); QVERIFY(menu); auto *filter=action(menu,"filterOnlyProject"); QVERIFY(filter);
    QCOMPARE(filter->text(),QStringLiteral("Filter this project"));
    QVERIFY(!action(menu,"toggleProjectFilter")); QVERIFY(!action(menu,"clearProjectFilter"));
    filter->trigger(); menu->hide();
    QCOMPARE(projects->selection(),QSet<QString>{work});
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    menu=openMenu(work); QVERIFY(menu); QVERIFY(!action(menu,"toggleProjectFilter")); QVERIFY(!action(menu,"clearProjectFilter"));
    action(menu,"filterOnlyProject")->trigger(); menu->hide(); QCOMPARE(projects->selection(),QSet<QString>{work});
    auto *chip=projects->findChild<QPushButton *>("projectFilterChip"); QVERIFY(chip); chip->click(); QVERIFY(projects->selection().isEmpty());
    projects->setSelection({work,docs}); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    menu=openMenu(docs); QVERIFY(menu); action(menu,"filterOnlyProject")->trigger(); menu->hide(); QCOMPARE(projects->selection(),QSet<QString>{docs});
    auto *chooser=projects->button()->menu(); chooser->popup(window.mapToGlobal(QPoint(100,100))); QTRY_VERIFY(chooser->isVisible());
    auto *all=chooser->findChild<QPushButton *>("allProjectFilters"); QVERIFY(all); all->click(); chooser->hide(); QVERIFY(projects->selection().isEmpty());
}

void TestSessionsWindow::dashboardAndMultiMachineNavigation()
{
    SessionsWindow window(program()); auto state=fleet(); auto third=state.local(); third.host="build"; state.setPeer(third,0);
    window.setFleet(state); window.show();
    auto *filter=window.findChild<MachineFilter *>("machineFilterChips"); auto *list=window.findChild<SessionList *>("sessionList");
    QVERIFY(filter); QVERIFY(list); filter->setSelection({"mac","build"}); QCOMPARE(list->count(),5);
    for(int i=0;i<list->count();++i) QVERIFY(!list->item(i)->data(SessionRoles::Identity).toString().startsWith("arch\n"));
    auto *brand=window.findChild<QPushButton *>("brandMark"); QVERIFY(brand); brand->click();
    auto *dashboard=window.findChild<DashboardPage *>(); QVERIFY(dashboard->isVisible()); QVERIFY(brand->isChecked()); brand->click(); QVERIFY(brand->isChecked());
    QVERIFY(!window.findChild<QPushButton *>("organizeGroups"));
    bool launchDialogOpened = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());
        launchDialogOpened = dialog != nullptr;
        if (dialog) dialog->reject();
    });
    QVERIFY(!dashboard->findChild<QPushButton *>("dashboardNewSession"));
    auto *create=window.findChild<QPushButton *>("newSession");QVERIFY(create->isVisible());
    QCOMPARE(create->parentWidget()->objectName(),QString("sidebar"));
    create->click(); QVERIFY(launchDialogOpened);
    dashboard->filterRequested("@all","attention"); QVERIFY(list->isVisible()); QCOMPARE(list->count(),1);
    QVERIFY(filter->selection().isEmpty()); QVERIFY(!brand->isChecked());
    brand->click(); dashboard->filterRequested("@local","all"); QCOMPARE(list->count(),4);
    QCOMPARE(filter->selection(),QSet<QString>{"@local"});
    brand->click(); dashboard->sessionRequested("mac","claude/infra/review");
    QVERIFY(list->currentItem()->data(SessionRoles::Identity).toString().startsWith("mac\n"));
    brand->click(); dashboard->machinesRequested("mac"); auto *machines=window.findChild<MachinesPage *>(); QVERIFY(machines->isVisible());
    machines->accountsRequested("mac"); auto *accounts=window.findChild<AccountsPage *>(); QVERIFY(accounts->isVisible());
    QCOMPARE(accounts->findChild<QComboBox *>("accountMachineFilter")->currentData().toString(),QString("mac"));
}

void TestSessionsWindow::pollsKeepUnchangedActivity()
{
    // Fleet polls and repeated inspections of an unchanged session leave Activity alone.
    SessionsWindow window(program()); auto state = fleet(); window.setFleet(state); window.show();
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    window.showSession({}, "codex/hgs/dashboard"); QTRY_VERIFY(!inspected.isEmpty());
    auto *activity = window.findChild<ActivityView *>("mainActivity"); QVERIFY(activity); QTRY_VERIFY(!activity->renderPending());
    const int passes = activity->renderPasses();
    QJsonObject details{{"tracked", true}, {"conversation_id", "conversation-one"}, {"events", QJsonArray{}}, {"cursor", 0}, {"goal_observed_at", 200.}};
    window.setFleet(state); client->inspectionReady({}, "codex/hgs/dashboard", details);
    details["goal_observed_at"] = 205.; client->inspectionReady({}, "codex/hgs/dashboard", details); window.setFleet(state);
    QCOMPARE(activity->renderPasses(), passes);
    details["cursor"] = 1; details["events"] = QJsonArray{QJsonObject{{"seq", 1}, {"type", "Stop"}, {"detail", "Fresh reply"}, {"at", 10.}}};
    client->inspectionReady({}, "codex/hgs/dashboard", details); QTRY_VERIFY(activity->renderPasses() > passes);
    QTRY_VERIFY(activity->plainText().contains("Fresh reply"));
    // Process links depend on a setting that the next render must follow.
    const int linked = activity->renderPasses(); QSettings().setValue("processes/enabled", true); window.setFleet(state);
    QTRY_VERIFY(activity->renderPasses() > linked);
}

void TestSessionsWindow::hiddenPagesRenderWhenShown()
{
    // Polls keep hidden pages' data current; each page renders when it is shown.
    SessionsWindow window(program()); auto state = fleet(); window.setFleet(state); window.show();
    auto *list = window.findChild<SessionList *>("sessionList"); auto *dashboard = window.findChild<DashboardPage *>();
    auto *badge = window.findChild<QLabel *>("railAttentionBadge"); auto *sessionsNav = qobject_cast<QPushButton *>(badge->parentWidget());
    auto *brand = window.findChild<QPushButton *>("brandMark"); auto *total = dashboard->findChild<QPushButton *>("dashboardSessions");
    QVERIFY(list); QVERIFY(sessionsNav); QVERIFY(brand); QVERIFY(total);
    const int rows = list->count(); const auto attention = badge->text();
    brand->click(); QVERIFY(dashboard->isVisible()); const auto before = total->text();
    auto box = state.local(); auto extra = box.sessions[0]; extra.name = "codex/hgs/extra"; extra.tag = "extra"; extra.phase = "approval";
    box.sessions.append(extra); state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QSignalSpy inserted(list->model(), &QAbstractItemModel::rowsInserted);
    window.setFleet(state);
    QCOMPARE(list->count(), rows); QCOMPARE(inserted.size(), 0);
    QVERIFY(total->text() != before); QCOMPARE(badge->text(), QString::number(attention.toInt() + 1));
    sessionsNav->click(); QVERIFY(list->isVisible()); QCOMPARE(list->count(), rows + 1);
    const int shown = inserted.size(); QVERIFY(shown > 0); QTest::qWait(20); QCOMPARE(inserted.size(), shown);
    const auto updated = total->text();
    box.sessions.removeLast(); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->count(), rows); QCOMPARE(total->text(), updated);
    brand->click(); QCOMPARE(total->text(), before);
    // A closed window renders its current page when it opens again.
    window.hide(); box.sessions.append(extra); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    window.show(); QCOMPARE(total->text(), updated);
}

void TestSessionsWindow::dashboardActivityRowsStayReadable_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark") << QString("dark");
    QTest::newRow("light") << QString("light");
}

void TestSessionsWindow::dashboardActivityRowsStayReadable()
{
    QFETCH(QString, theme);
    QSettings().setValue("workspace/theme", theme);
    SessionsWindow window(program()); window.resize(1170, 660); window.show();
    window.findChild<QPushButton *>("brandMark")->click();
    auto state = fleet(); auto local = state.local(); auto &session = local.sessions[0];
    session.gitBranch = "main"; session.turnStarted = QDateTime::currentSecsSinceEpoch() - 76;
    session.subagentSource = "hooks"; session.subagentCountsComplete = true;
    session.subagentActiveCount = 2; session.subagentTotalCount = 4;
    state.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    QPointer<QPushButton> workingRow;
    // The real window stylesheet and repeated fleet updates must retain both
    // card geometry and identity/focus. Dashboard cards have no roster control.
    for (int poll = 0; poll < 3; ++poll) {
        window.setFleet(state); QTest::qWait(20);
        const auto rows = window.findChildren<QPushButton *>("dashboardSessionRow");
        QCOMPARE(rows.size(), 2);
        for (auto *row : rows) {
            QVERIFY2(row->height() >= 104, qPrintable(QString("Activity row collapsed to %1px").arg(row->height())));
            auto *model = row->findChild<QStandardItemModel *>("dashboardSessionModel");
            QVERIFY(model); const auto index = model->index(0, 0);
            for (const int role : {SessionRoles::Title, SessionRoles::Status,
                    SessionRoles::Agent, SessionRoles::Host, SessionRoles::Detail, SessionRoles::Model, SessionRoles::Effort})
                QVERIFY(!index.data(role).toString().isEmpty());
            QVERIFY(!index.data(SessionRoles::HasChildren).toBool());
            if (index.data(SessionRoles::Working).toBool()) {
                QCOMPARE(index.data(SessionRoles::Title).toString(), QString("dashboard"));
                QCOMPARE(index.data(SessionRoles::Meta).toString(), QString("main"));
                QCOMPARE(index.data(SessionRoles::MetaIcon).toString(), QString("git-branch"));
                QCOMPARE(index.data(SessionRoles::WorkingSince).toDouble(), session.turnStarted);
                QCOMPARE(index.data(SessionRoles::Children).toString(), QString("2"));
                if (poll == 0) { workingRow = row; row->setFocus(); }
                else { QCOMPARE(row, workingRow.data()); QVERIFY(row->hasFocus()); }
            } else QCOMPARE(index.data(SessionRoles::Meta).toString(), QString());
        }
    }
    const auto directory = qEnvironmentVariable("HGS_DASHBOARD_PREVIEW");
    if (!directory.isEmpty()) { QDir().mkpath(directory); QVERIFY(window.grab().save(directory + "/integrated-" + theme + ".png")); QVERIFY(workingRow->grab().save(directory + "/card-" + theme + ".png")); }
    QVERIFY(workingRow);
    // Even the subagent-count area navigates; it never expands a dashboard row.
    QTest::mouseClick(workingRow, Qt::LeftButton, Qt::NoModifier, QPoint(workingRow->width() - 30, workingRow->height() - 20));
    auto *list = window.findChild<SessionList *>("sessionList");
    QVERIFY(list->isVisible());
    QVERIFY(list->currentItem()->data(SessionRoles::Identity).toString().contains("codex/hgs/dashboard"));

}

void TestSessionsWindow::multiSelectionKeepsConversationAndGroupMenu_data()
{
    QTest::addColumn<QString>("theme");QTest::addColumn<int>("width");
    QTest::newRow("dark")<<QString("dark")<<1100;QTest::newRow("light-narrow")<<QString("light")<<840;
}
void TestSessionsWindow::multiSelectionKeepsConversationAndGroupMenu()
{
    QFETCH(QString,theme);QFETCH(int,width);QSettings().setValue("workspace/theme",theme);
    auto state=fleet();SessionsWindow window(program());window.resize(width,760);window.setFleet(state);window.show();
    if(width<900)window.findChild<QSplitter *>()->setSizes({270,width-270});
    auto *list=window.findChild<SessionList *>("sessionList");auto *composer=window.findChild<MessageComposer *>();
    const auto open=list->currentItem()->data(SessionRoles::Key).toString();
    setComposerText(composer, "Keep this draft");
    const auto select=[&](int row){QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::ControlModifier,list->visualItemRect(list->item(row)).center());};
    select(1);QCOMPARE(list->selectedItems().size(),2);QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(),open);
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    auto *bar=window.findChild<QFrame *>("sessionSelectionBar");QVERIFY(bar);QVERIFY(bar->isVisible());
    for(auto *button:bar->findChildren<QToolButton *>())if(button->isVisible()) {
        QVERIFY(button->width()>=24);QVERIFY(bar->rect().contains(QRect(button->mapTo(bar,QPoint()),button->size())));
    }
    QSet<QString> keys;for(auto *item:list->selectedItems())keys.insert(item->data(SessionRoles::Key).toString());
    // Reordering the actual rows must preserve the chosen identities.
    auto local=state.local();std::swap(local.sessions[1],local.sessions[2]);state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    QSet<QString> after;for(auto *item:list->selectedItems())after.insert(item->data(SessionRoles::Key).toString());QCOMPARE(after,keys);
    QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(),open);QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    const auto point=list->visualItemRect(list->item(2)).center();
    QTest::mouseClick(list->viewport(),Qt::RightButton,{},point);
    QContextMenuEvent event(QContextMenuEvent::Mouse,point,list->viewport()->mapToGlobal(point));QApplication::sendEvent(list->viewport(),&event);
    auto *menu=window.findChild<QMenu *>("sessionContextMenu");QVERIFY(menu->isVisible());
    QVERIFY(!menu->findChild<QAction *>("contextOpenTerminal"));QVERIFY(menu->findChild<QMenu *>("moveSelectionToProject"));
    QVERIFY(menu->findChild<QAction *>("selectionattention"));QCOMPARE(list->selectedItems().size(),2);menu->hide();
    const auto preview=qEnvironmentVariable("HGS_BULK_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/selection-"+theme+".png"));}
    bar->findChild<QPushButton *>("clearSessionSelection")->click();QVERIFY(list->selectedItems().isEmpty());QVERIFY(!bar->isVisible());
    window.setFleet(state);QVERIFY(list->selectedItems().isEmpty());QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    QTest::mouseClick(list->viewport(),Qt::LeftButton,{},list->visualItemRect(list->item(0)).center());QCOMPARE(list->selectedItems().size(),1);
    select(1);QCOMPARE(list->selectedItems().size(),2);QTest::keyClick(list,Qt::Key_Escape);QVERIFY(list->selectedItems().isEmpty());
}

void TestSessionsWindow::selectionCommandsKeepPinnedTargets()
{
    auto state=fleet();auto box=state.local();box.sessions[0].activity="idle";box.sessions[0].phase="idle";
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());auto remote=*state.peer("mac");remote.sessions[0].activity="idle";remote.sessions[0].phase="idle";
    remote.sessions[0].runId="original";state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.resize(1100,850);window.setFleet(state);window.show();
    auto *list=window.findChild<SessionList *>("sessionList");
    QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::ControlModifier,list->visualItemRect(list->item(4)).center());
    QCOMPARE(list->selectedItems().size(),2);
    QTest::keyClick(list,Qt::Key_Menu);auto *menu=window.findChild<QMenu *>("sessionContextMenu");
    auto *pause=menu->findChild<QAction *>("selectionpause");QVERIFY(pause);QCOMPARE(pause->text(),QString("Pause (2)"));
    auto *client=window.findChild<HgsClient *>();QSignalSpy writes(client,&HgsClient::writeDone);
    // A native restart after opening the group menu must not retarget its action.
    remote.sessions[0].runId="replacement";state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    pause->trigger();menu->hide();QTRY_COMPARE(writes.size(),1);
    QCOMPARE(writes[0][2].toString(),QString("pause\ncodex/hgs/dashboard"));
    QTRY_VERIFY(window.findChild<QLabel *>("notice")->text().contains("1 failed or changed"));
    // A fresh selection is allowed to act on both current instances.
    auto *control=window.findChild<QAction *>("selectionpause");QVERIFY(control);QTRY_VERIFY(control->isEnabled());control->trigger();
    QTRY_COMPARE(writes.size(),3);QCOMPARE(writes[2][2].toString(),QString("@mac\npause\nclaude/infra/review"));
}

void TestSessionsWindow::clearArchiveKeepsLiveAndNewEntries()
{
    auto state=fleet();auto box=state.local();auto archived=box.sessions[0];archived.state="archived";archived.archiveId="one";
    auto second=archived;second.archiveId="two";box.sessions<<archived<<second;state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    auto remote=*state.peer("mac");auto remoteArchive=archived;remoteArchive.archiveId="remote";remote.sessions<<remoteArchive;state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
    remote.ok=false;state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.setFleet(state);window.show();
    auto *clear=window.findChild<QAction *>("clearArchive");QVERIFY(clear);
    auto *context=window.findChild<QMenu *>("sessionProjectsMenu");QVERIFY(context);QVERIFY(context->actions().contains(clear));
    auto *batch=window.findChild<QPushButton *>("batchActions")->menu();
    // Archive cleanup is available from every filter, including the live list.
    for(auto *button:window.findChildren<QPushButton *>("sessionFilter")) {
        button->click();QVERIFY(clear->isVisible());QVERIFY(clear->isEnabled());
        QMetaObject::invokeMethod(batch,"aboutToShow");QVERIFY(batch->actions().contains(clear));
    }
    window.showSession({},box.sessions.first().name);
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({"mac"});QVERIFY(clear->isVisible());QVERIFY(!clear->isEnabled());
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({});QVERIFY(clear->isEnabled());
    auto *client=window.findChild<HgsClient *>();QSignalSpy writes(client,&HgsClient::writeDone);
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("confirmSelectionAction");QVERIFY(dialog);dialog->reject();});clear->trigger();QCOMPARE(writes.size(),0);
    QTimer::singleShot(0,&window,[&]{
        auto *dialog=window.findChild<QMessageBox *>("confirmSelectionAction");QVERIFY(dialog);
        QVERIFY(dialog->text().contains("2 archived"));QVERIFY(dialog->text().contains("1 unavailable"));
        auto newer=archived;newer.archiveId="created-during-confirmation";box.sessions<<newer;state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();break;}
    });clear->trigger();QTRY_COMPARE(writes.size(),2);
    QStringList payloads;for(const auto &write:writes)payloads<<write[2].toString();
    QVERIFY(payloads.contains("kill\ncodex/hgs/dashboard\n--archive\none"));QVERIFY(payloads.contains("kill\ncodex/hgs/dashboard\n--archive\ntwo"));
    QTest::qWait(30);QCOMPARE(writes.size(),2);
}

void TestSessionsWindow::groupsPersistFilterAndRevealAttention()
{
    SessionOrganization org;
    const auto work = org.createGroup("Release"), docs = org.createGroup("Research");
    const auto local = org.observe("arch", "codex/hgs/dashboard", ""), remote = org.observe("mac", "claude/infra/review", "");
    org.moveSession(local, work); org.moveSession(remote, work); org.setCollapsed(work, true);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); window.setFleet(fleet()); window.show();
    auto *list = window.findChild<SessionList *>("sessionList"); QVERIFY(list);
    const auto header = [&](const QString &id) -> QListWidgetItem * {
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::Header).toBool() && list->item(i)->data(SessionRoles::Group).toString() == id) return list->item(i);
        return nullptr;
    };
    QVERIFY(header(work)); QCOMPARE(header(work)->data(SessionRoles::Attention).toInt(), 1);
    window.showAttentionSession("mac", "claude/infra/review");
    QCOMPARE(list->currentItem()->data(SessionRoles::Identity).toString(), remote);
    QVERIFY(!list->currentItem()->isHidden()); QVERIFY(!header(work)->data(SessionRoles::Collapsed).toBool());
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(header(work)).center());
    QVERIFY(header(work)->data(SessionRoles::Collapsed).toBool());
    QCOMPARE(list->currentItem()->data(SessionRoles::Identity).toString(), remote); // Keep the working pane when collapsing its group.
    auto *search = window.findChild<QLineEdit *>("search"); search->setText("review");
    auto *results = window.findChild<QListWidget *>("searchResults"); QVERIFY(results->isVisible()); QCOMPARE(results->count(), 1);
    search->clear(); QVERIFY(list->currentItem()->isHidden());
    list->sessionMoved(remote, docs, {}); list->groupMoved(docs, work);
    auto persisted = SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(persisted.groupFor(remote), docs); QCOMPARE(persisted.groups().at(0).id, docs);
    auto offline = fleet(); BoxState unavailable; unavailable.host = "mac"; offline.setPeer(unavailable, QDateTime::currentMSecsSinceEpoch()); window.setFleet(offline);
    QCOMPARE(header(docs)->data(SessionRoles::Attention).toInt(), 0); QCOMPARE(list->currentItem()->data(SessionRoles::Identity).toString(), remote);
    window.close(); SessionsWindow reopened(program()); reopened.setFleet(fleet()); reopened.show();
    reopened.showAttentionSession("mac", "claude/infra/review");
    auto *newList = reopened.findChild<SessionList *>("sessionList"); QCOMPARE(newList->currentItem()->data(SessionRoles::Group).toString(), docs);
    QVERIFY(!newList->currentItem()->isHidden());
}

void TestSessionsWindow::projectSwarmFollowsWorkspaceTheme()
{
    const QJsonObject organization{{"version",2},{"projects",QJsonArray{}}};
    for(const auto &theme:{QStringLiteral("dark"),QStringLiteral("light")}) {
        QSettings().setValue("workspace/theme",theme);SessionsWindow window(program());window.show();
        HgsClient client(program());SwarmController controller(&client);
        controller.acceptExternal({{"schema",1},{"organization",organization},{"machines",QJsonArray{}},{"conflicts",QJsonArray{}}});
        FleetState fleet;bool checked=false;
        QTimer::singleShot(30,&window,[&]{
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());QVERIFY(dialog);
            QCOMPARE(dialog->palette().color(QPalette::Window),window.palette().color(QPalette::Window));
            checked=true;dialog->reject();
        });
        QTimer timeout;timeout.setSingleShot(true);connect(&timeout,&QTimer::timeout,&window,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()))dialog->reject();});timeout.start(5000);
        showSwarmDialog(&client,&controller,fleet,&window,[]{});QVERIFY(checked);
    }
}

void TestSessionsWindow::contentScaleLeavesWorkspaceChrome()
{
    QSettings().remove("workspace/contentScale");
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
    window.showSession({}, "codex/hgs/dashboard");
    const auto preview = [&window](const QString &name) {
        const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR"); if (directory.isEmpty()) return true;
        window.showSession({}, "codex/hgs/dashboard");
        const QJsonObject details{{"tracked", true}, {"run_id", "run"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
            {"process_state", "running"}, {"activity", "idle"}, {"phase", "idle"}, {"cursor", 2}, {"events", QJsonArray{
                QJsonObject{{"seq", 1}, {"at", 2000000000}, {"type", "UserPromptSubmit"}, {"detail", "Make the transcript easier to read."}},
                QJsonObject{{"seq", 2}, {"at", 2000000010}, {"type", "Stop"}, {"detail", "## Done\n\nContent now follows **Settings → Appearance**. Run `ctest` to verify."}}}}};
        window.findChild<HgsClient *>()->inspectionReady({}, "codex/hgs/dashboard", details); QTest::qWait(50);
        return window.grab().save(directory + "/" + name);
    };
    QVERIFY(preview("content-scale-100.png"));
    const auto pixels = [&window](const QString &name) {
        auto *widget = window.findChild<QWidget *>(name); return widget ? QFontInfo(widget->font()).pixelSize() : -1;
    };
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    const auto editorPixels = [composer] { return QFontInfo(composer->editor()->font()).pixelSize(); };
    const QStringList chrome{"sessionList", "search", "detailTitle", "sessionInspector", "sessionInfo", "terminalStatus", "sendMessage"};
    QMap<QString, int> before; for (const auto &name : chrome) { before.insert(name, pixels(name)); QVERIFY2(before.value(name) > 0, qPrintable(name)); }
    const int tabs = QFontInfo(window.findChild<QTabWidget *>("sessionDetailTabs")->tabBar()->font()).pixelSize();
    QCOMPARE(pixels("activity"), 13); QCOMPARE(pixels("terminalScreen"), 13); QCOMPARE(editorPixels(), 13);
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *settings = window.findChild<QWidget *>("settingsPage"); QVERIFY(settings && settings->isVisible());
    auto *slider = settings->findChild<QSlider *>("workspaceContentScale"); QVERIFY(slider && slider->isVisible());
    QCOMPARE(slider->minimum(), 15); QCOMPARE(slider->maximum(), 40); QCOMPARE(slider->value(), 20);
    QCOMPARE(settings->findChild<QLabel *>("workspaceContentScaleValue")->text(), QString("100%"));
    slider->setValue(15);
    QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 0.75);
    QCOMPARE(settings->findChild<QLabel *>("workspaceContentScaleValue")->text(), QString("75%"));
    QTRY_COMPARE(editorPixels(), 10);
    QCOMPARE(pixels("activity"), 10); QCOMPARE(pixels("subagentJournal"), 10); QCOMPARE(pixels("terminalScreen"), 10);
    for (const auto &name : chrome) QCOMPARE(pixels(name), before.value(name));
    QCOMPARE(QFontInfo(window.findChild<QTabWidget *>("sessionDetailTabs")->tabBar()->font()).pixelSize(), tabs);
    QVERIFY(preview("content-scale-75.png"));
    window.findChild<QPushButton *>("workspaceSettings")->click();
    slider->setValue(30); slider->setValue(40);
    QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 2.0);
    QCOMPARE(settings->findChild<QLabel *>("workspaceContentScaleValue")->text(), QString("200%"));
    QTRY_COMPARE(editorPixels(), 26);
    if (!qEnvironmentVariable("HGS_PREVIEW_DIR").isEmpty())
        QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PREVIEW_DIR") + "/content-scale-settings.png"));
    QCOMPARE(pixels("activity"), 26); QCOMPARE(pixels("subagentJournal"), 26); QCOMPARE(pixels("terminalScreen"), 26);
    for (const auto &name : chrome) QCOMPARE(pixels(name), before.value(name));
    QCOMPARE(QFontInfo(window.findChild<QTabWidget *>("sessionDetailTabs")->tabBar()->font()).pixelSize(), tabs);
    // Theme changes rebuild workspace styles without dropping the preference.
    QSettings().setValue("workspace/theme", "dark"); settings->findChild<QComboBox *>("workspaceTheme")->setCurrentIndex(1);
    QCOMPARE(editorPixels(), 26); QCOMPARE(pixels("activity"), 26);
    QVERIFY(preview("content-scale-200.png"));
    SessionsWindow restored(program()); restored.setFleet(fleet());
    QCOMPARE(QFontInfo(restored.findChild<MessageComposer *>("messageComposer")->editor()->font()).pixelSize(), 26);
    QSettings().setValue("workspace/contentScale", 2.5);
    SessionsWindow legacy(program()); legacy.setFleet(fleet());
    QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 2.0);
    QCOMPARE(QFontInfo(legacy.findChild<MessageComposer *>("messageComposer")->editor()->font()).pixelSize(), 26);
    window.findChild<QPushButton *>("workspaceSettings")->click(); slider->setValue(20);
    QTRY_COMPARE(editorPixels(), 13);
    QCOMPARE(pixels("activity"), 13); QCOMPARE(pixels("terminalScreen"), 13);
}

void TestSessionsWindow::activitySurfaceRemainsOpaque_data()
{
    QTest::addColumn<QString>("theme"); QTest::addColumn<double>("scale");
    for (const QString theme : {QString("dark"), QString("light")})
        for (double scale : {1.0, 1.25})
            QTest::newRow(qPrintable(theme+QString::number(scale))) << theme << scale;
}

void TestSessionsWindow::activitySurfaceRemainsOpaque()
{
    QFETCH(QString,theme); QFETCH(double,scale);
    QSettings().setValue("workspace/theme",theme); QSettings().setValue("workspace/contentScale",scale);
    SessionsWindow window(program()); window.resize(1180,820); window.setFleet(fleet()); window.show();
    window.showSession({},"codex/hgs/dashboard");
    const auto views=window.findChildren<ActivityView *>(); QVERIFY(views.size()>=2);
    for (auto *view : views) {
        auto *browser=view->browser(); browser->ensurePolished(); browser->viewport()->ensurePolished();
        const auto base=browser->viewport()->palette().brush(QPalette::Base);
        QVERIFY2(base.isOpaque(),"Workspace CSS disabled the fast Activity canvas renderer");
        QCOMPARE(base.color(),view->palette().color(QPalette::Window));
    }
}

void TestSessionsWindow::activityReadingColumnFollowsSettingsAndEdges()
{
    SessionsWindow window(program()); window.resize(2200, 900); window.setFleet(fleet()); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto open = [&window] {
        window.showSession({}, "codex/hgs/dashboard");
        const QJsonObject details{{"tracked", true}, {"run_id", "run"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
            {"process_state", "running"}, {"activity", "idle"}, {"phase", "idle"}, {"cursor", 2}, {"events", QJsonArray{
                QJsonObject{{"seq", 1}, {"at", 2000000000}, {"type", "UserPromptSubmit"}, {"detail", "Keep long lines readable in a maximized window."}},
                QJsonObject{{"seq", 2}, {"at", 2000000010}, {"type", "Stop"}, {"detail", "## Done\n\nActivity, questions and the message field now share **one centered column**."}}}}};
        window.findChild<HgsClient *>()->inspectionReady({}, "codex/hgs/dashboard", details);
    };
    const auto preview = [&window, &open](const QString &name) {
        const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR"); if (directory.isEmpty()) return true;
        open(); QTest::qWait(50); return window.grab().save(directory + "/" + name);
    };
    open();
    auto *activity = window.findChild<ActivityView *>("mainActivity"); QVERIFY(activity);
    auto *viewport = activity->browser()->viewport();
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    const auto left = [&window](QWidget *widget) { return widget->mapTo(&window, QPoint()).x(); };
    // A wide pane centers the transcript and the message field in one column.
    QTRY_COMPARE(viewport->width(), 900);
    const int pane = activity->width(); QVERIFY2(pane > 1500, qPrintable(QString::number(pane)));
    QCOMPARE(left(viewport) - left(activity), (pane - 900) / 2);
    QTRY_COMPARE(composer->width(), 900); QCOMPARE(left(composer), left(viewport));
    // The margins are Activity's own surface in either theme.
    const auto plainMargins = [activity, viewport] {
        const QImage surface = activity->grab().toImage();
        // The document margin inside the column is always its background.
        const QPoint empty = viewport->mapTo(activity, QPoint(1, viewport->height() / 2));
        return surface.pixelColor(20, empty.y()) == surface.pixelColor(empty)
            && surface.pixelColor(activity->width() - 30, empty.y()) == surface.pixelColor(empty);
    };
    QVERIFY(plainMargins());
    QVERIFY(preview("activity-column.png"));
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *settings = window.findChild<QWidget *>("settingsPage"); QVERIFY(settings && settings->isVisible());
    auto *slider = settings->findChild<QSlider *>("workspaceActivityWidth"); QVERIFY(slider && slider->isVisible());
    auto *value = settings->findChild<QLabel *>("workspaceActivityWidthValue"); QVERIFY(value);
    auto *full = settings->findChild<QCheckBox *>("workspaceActivityFullWidth"); QVERIFY(full && !full->isChecked());
    QCOMPARE(slider->minimum(), 480); QCOMPARE(slider->maximum(), 2400); QCOMPARE(slider->value(), 900);
    QCOMPARE(value->text(), QString("900 px"));
    slider->setValue(1100);
    QCOMPARE(QSettings().value("workspace/activityWidth").toInt(), 1100); QCOMPARE(value->text(), QString("1100 px"));
    if (!qEnvironmentVariable("HGS_PREVIEW_DIR").isEmpty())
        QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PREVIEW_DIR") + "/activity-width-settings.png"));
    open();
    QTRY_COMPARE(viewport->width(), 1100); QTRY_COMPARE(composer->width(), 1100); QCOMPARE(left(composer), left(viewport));
    // Enlarged content keeps the same line length.
    window.findChild<QPushButton *>("workspaceSettings")->click();
    settings->findChild<QSlider *>("workspaceContentScale")->setValue(30);
    open();
    QTRY_COMPARE(viewport->width(), 1650); QTRY_COMPARE(composer->width(), 1650);
    window.findChild<QPushButton *>("workspaceSettings")->click();
    settings->findChild<QSlider *>("workspaceContentScale")->setValue(20);
    full->setChecked(true);
    QVERIFY(QSettings().value("workspace/activityFullWidth").toBool()); QVERIFY(!slider->isEnabled());
    open();
    QTRY_COMPARE(composer->width(), pane); QCOMPARE(left(viewport), left(activity));
    QVERIFY(preview("activity-full-width.png"));
    // Activity's own menu returns to the column and Settings follows.
    emit activity->fullWidthRequested(false);
    QTRY_COMPARE(viewport->width(), 1100); QTRY_COMPARE(composer->width(), 1100);
    QVERIFY(!QSettings().value("workspace/activityFullWidth").toBool()); QVERIFY(!full->isChecked()); QVERIFY(slider->isEnabled());
    // Dragging a column edge stores the width for both Activity views.
    auto edges = activity->browser()->findChildren<QWidget *>("activityColumnEdge");
    std::sort(edges.begin(), edges.end(), [](QWidget *a, QWidget *b) { return a->x() < b->x(); });
    QCOMPARE(edges.size(), 2); QVERIFY(edges[1]->isVisible());
    if (!qEnvironmentVariable("HGS_PREVIEW_DIR").isEmpty()) {
        QEnterEvent enter(QPointF(4, 100), edges[1]->mapToGlobal(QPointF(4, 100)), edges[1]->mapToGlobal(QPointF(4, 100)));
        QApplication::sendEvent(edges[1], &enter);
        QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PREVIEW_DIR") + "/activity-column-edge.png"));
        QEvent leave(QEvent::Leave); QApplication::sendEvent(edges[1], &leave);
    }
    const QPoint start = edges[1]->mapToGlobal(QPoint(edges[1]->width() / 2, 100));
    const auto mouse = [](QWidget *widget, QEvent::Type type, const QPoint &global, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, widget->mapFromGlobal(QPointF(global)), QPointF(global), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    };
    mouse(edges[1], QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(edges[1], QEvent::MouseMove, start + QPoint(60, 0), Qt::NoButton, Qt::LeftButton);
    mouse(edges[1], QEvent::MouseMove, start + QPoint(100, 0), Qt::NoButton, Qt::LeftButton);
    mouse(edges[1], QEvent::MouseButtonRelease, start + QPoint(100, 0), Qt::LeftButton, Qt::NoButton);
    QCOMPARE(viewport->width(), 1300); QTRY_COMPARE(composer->width(), 1300); QCOMPARE(left(composer), left(viewport));
    QCOMPARE(QSettings().value("workspace/activityWidth").toInt(), 1300);
    QCOMPARE(slider->value(), 1300); QCOMPARE(value->text(), QString("1300 px"));
    QCOMPARE(window.findChild<ActivityView *>("subagentActivity")->columnWidth(), 1300);
    emit activity->columnResetRequested();
    QTRY_COMPARE(viewport->width(), 900); QCOMPARE(slider->value(), 900);
    QCOMPARE(QSettings().value("workspace/activityWidth").toInt(), 900);
    settings->findChild<QComboBox *>("workspaceTheme")->setCurrentIndex(1);
    open(); QTRY_COMPARE(composer->width(), 900); QVERIFY(plainMargins());
    QVERIFY(preview("activity-column-dark.png"));
    SessionsWindow restored(program()); restored.setFleet(fleet());
    QCOMPARE(restored.findChild<ActivityView *>("mainActivity")->columnWidth(), 900);
    full->setChecked(true);
    SessionsWindow fullWidth(program()); fullWidth.setFleet(fleet());
    QCOMPARE(fullWidth.findChild<ActivityView *>("mainActivity")->columnWidth(), 0);
    QCOMPARE(fullWidth.findChild<ActivityView *>("subagentActivity")->columnWidth(), 0);
}

void TestSessionsWindow::settingsSlidersResetToDefaults()
{
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *settings = window.findChild<QWidget *>("settingsPage"); QVERIFY(settings && settings->isVisible());
    auto *scale = settings->findChild<QSlider *>("workspaceContentScale"), *width = settings->findChild<QSlider *>("workspaceActivityWidth");
    auto *resetScale = settings->findChild<QPushButton *>("resetContentScale"), *resetWidth = settings->findChild<QPushButton *>("resetActivityWidth");
    auto *full = settings->findChild<QCheckBox *>("workspaceActivityFullWidth");
    QVERIFY(scale && width && full && resetScale && resetWidth);
    QCOMPARE(resetScale->toolTip(), QString("Reset to 100%")); QCOMPARE(resetWidth->toolTip(), QString("Reset to 900 px"));
    // Defaults leave nothing to reset, so the buttons appear only after a change.
    QVERIFY(!resetScale->isVisible()); QVERIFY(!resetWidth->isVisible());
    const int below = full->mapTo(settings, QPoint()).y();
    scale->setValue(30); width->setValue(1300);
    QVERIFY(resetScale->isVisible()); QVERIFY(resetWidth->isVisible());
    // Appearing does not move the rows beneath.
    QTest::qWait(50); QCOMPARE(full->mapTo(settings, QPoint()).y(), below);
    // Icon buttons right after each value, named by the default they restore.
    const QList<QPair<QPushButton *, QLabel *>> rows{{resetScale, settings->findChild<QLabel *>("workspaceContentScaleValue")},
                                                     {resetWidth, settings->findChild<QLabel *>("workspaceActivityWidthValue")}};
    for (const auto &[button, value] : rows) {
        QVERIFY(button->text().isEmpty()); QVERIFY(!button->icon().isNull());
        QCOMPARE(button->property("glyph").toString(), QString("reset"));
        QCOMPARE(button->accessibleName(), button->toolTip());
        const auto center = [settings](QWidget *widget) { return widget->mapTo(settings, widget->rect().center()); };
        QVERIFY(qAbs(center(button).y() - center(value).y()) <= 2);
        const int textEnd = value->mapTo(settings, QPoint()).x() + value->fontMetrics().horizontalAdvance(value->text());
        const int gap = button->mapTo(settings, QPoint()).x() - textEnd;
        QVERIFY2(gap >= 0 && gap <= 8, qPrintable(QString("%1: %2").arg(button->objectName()).arg(gap)));
    }
    if (const auto directory = qEnvironmentVariable("HGS_PREVIEW_DIR"); !directory.isEmpty()) {
        auto *theme = settings->findChild<QComboBox *>("workspaceTheme");
        theme->setCurrentIndex(1); QVERIFY(window.grab().save(directory + "/settings-reset-dark.png"));
        theme->setCurrentIndex(2); QVERIFY(window.grab().save(directory + "/settings-reset-light.png"));
        theme->setCurrentIndex(0);
    }
    // Resetting from the keyboard leaves focus on the slider, not on a later control.
    resetScale->setFocus(Qt::TabFocusReason); QVERIFY(resetScale->hasFocus());
    QTest::keyClick(resetScale, Qt::Key_Space);
    QCOMPARE(scale->value(), 20); QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 1.0);
    QCOMPARE(settings->findChild<QLabel *>("workspaceContentScaleValue")->text(), QString("100%"));
    QVERIFY(!resetScale->isVisible()); QVERIFY(scale->hasFocus());
    QTest::mouseClick(resetWidth, Qt::LeftButton);
    QCOMPARE(width->value(), 900); QCOMPARE(QSettings().value("workspace/activityWidth").toInt(), 900);
    QCOMPARE(settings->findChild<QLabel *>("workspaceActivityWidthValue")->text(), QString("900 px"));
    QVERIFY(!resetWidth->isVisible());
    QTest::qWait(50); QCOMPARE(full->mapTo(settings, QPoint()).y(), below);
    // Full width has no column width to reset.
    width->setValue(1300); QVERIFY(resetWidth->isVisible());
    full->setChecked(true); QVERIFY(!resetWidth->isVisible());
    full->setChecked(false); QVERIFY(resetWidth->isVisible());
    // Activity's column edges and menu keep the button current.
    emit window.findChild<ActivityView *>("mainActivity")->columnResetRequested();
    QCOMPARE(width->value(), 900); QVERIFY(!resetWidth->isVisible());
    emit window.findChild<ActivityView *>("mainActivity")->columnWidthRequested(1200);
    QVERIFY(resetWidth->isVisible());
}

void TestSessionsWindow::alwaysOnTopSettingKeepsWindowAbove()
{
    // Off by default. The option applies in place: changing a visible window's
    // flags through QWidget would hide it from the user's workspace.
    const auto above = [](QWidget &window) { return window.windowHandle()->flags().testFlag(Qt::WindowStaysOnTopHint); };
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(!above(window));
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *option = window.findChild<QCheckBox *>("workspaceAlwaysOnTop");
    QVERIFY(option && option->isVisible() && option->isEnabled() && !option->isChecked());
    option->setChecked(true);
    QVERIFY(QSettings().value("workspace/alwaysOnTop").toBool());
    QVERIFY(above(window)); QVERIFY(window.windowFlags().testFlag(Qt::WindowStaysOnTopHint)); QVERIFY(window.isVisible());
    SessionsWindow restored(program()); restored.setFleet(fleet()); restored.show(); QVERIFY(QTest::qWaitForWindowExposed(&restored));
    QVERIFY(above(restored));
    option->setChecked(false);
    QVERIFY(!above(window)); QVERIFY(window.isVisible());
    QVERIFY(!QSettings().value("workspace/alwaysOnTop").toBool());
    QSettings().remove("workspace/alwaysOnTop");
}

void TestSessionsWindow::railPinTogglesAlwaysOnTop()
{
    // The pin above Settings switches the same preference as Settings → Appearance.
    const auto above = [](QWidget &window) { return window.windowHandle()->flags().testFlag(Qt::WindowStaysOnTopHint); };
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *pin = window.findChild<QPushButton *>("windowPin");
    QVERIFY(pin && pin->isVisible() && pin->isCheckable() && !pin->isChecked());
    const auto unpinnedIcon = pin->icon().pixmap(pin->iconSize()).toImage();
    const auto pinAction = pin->toolTip();
    pin->click();
    QVERIFY(pin->isChecked()); QVERIFY(above(window)); QVERIFY(window.isVisible());
    QVERIFY(pin->icon().pixmap(pin->iconSize()).toImage() != unpinnedIcon);
    QVERIFY(pin->toolTip() != pinAction); QCOMPARE(pin->accessibleName(), pin->toolTip());
    QVERIFY(QSettings().value("workspace/alwaysOnTop").toBool());
    auto *option = window.findChild<QCheckBox *>("workspaceAlwaysOnTop"); QVERIFY(option && option->isChecked());
    option->setChecked(false);
    QVERIFY(!pin->isChecked()); QVERIFY(!above(window));
    QCOMPARE(pin->icon().pixmap(pin->iconSize()).toImage(), unpinnedIcon); QCOMPARE(pin->toolTip(), pinAction);
    option->setChecked(true);
    QVERIFY(pin->isChecked()); QVERIFY(above(window));
    auto *theme = window.findChild<QComboBox *>("workspaceTheme"); QVERIFY(theme);
    const int originalTheme = theme->currentIndex();
    theme->setCurrentIndex(1); const auto darkPinned = pin->icon().pixmap(pin->iconSize()).toImage();
    theme->setCurrentIndex(2);
    QVERIFY(pin->isChecked()); QVERIFY(above(window));
    QVERIFY(pin->icon().pixmap(pin->iconSize()).toImage() != darkPinned);
    theme->setCurrentIndex(originalTheme);
    SessionsWindow restored(program()); restored.setFleet(fleet());
    QVERIFY(restored.findChild<QPushButton *>("windowPin")->isChecked());
    pin->click();
    QVERIFY(!option->isChecked()); QVERIFY(!above(window));
    QSettings().remove("workspace/alwaysOnTop");
}

void TestSessionsWindow::windowLayerControlsKeepFocusAndLayout_data()
{
    QTest::addColumn<QString>("controlName");
    QTest::addColumn<bool>("keyboard");
    QTest::newRow("checkbox-keyboard") << QString("workspaceAlwaysOnTop") << true;
    QTest::newRow("checkbox-mouse") << QString("workspaceAlwaysOnTop") << false;
    QTest::newRow("rail-pin-keyboard") << QString("windowPin") << true;
    QTest::newRow("rail-pin-mouse") << QString("windowPin") << false;
}

void TestSessionsWindow::windowLayerControlsKeepFocusAndLayout()
{
    QFETCH(QString, controlName); QFETCH(bool, keyboard);
    SessionsWindow window(program()); window.setFleet(fleet());
    // Give the synthetic window its own title, separate from the live workspace.
    window.setWindowTitle("Zerus window-layer controls " + QUuid::createUuid().toString(QUuid::Id128));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window)); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *layer = window.findChild<WindowLayer *>();
    auto *control = window.findChild<QAbstractButton *>(controlName);
    auto *hint = window.findChild<QLabel *>("workspaceAlwaysOnTopHint");
    QVERIFY(layer && control && hint);
    QTRY_VERIFY(layer->supported() && !layer->busy());
    QTest::qWait(30);
    const auto text = hint->text(); const auto geometry = hint->geometry();
    int pending = 0;
    bool pendingKeptFocus = true, pendingKeptLayout = true;
    connect(layer, &WindowLayer::changed, &window, [&] {
        if (!layer->busy()) return;
        ++pending;
        pendingKeptFocus &= control->isEnabled() && control->hasFocus();
        pendingKeptLayout &= hint->text() == text && hint->geometry() == geometry;
    });
    for (const bool on : {true, false}) {
        control->setFocus(Qt::TabFocusReason); QTRY_VERIFY(control->hasFocus());
        if (keyboard) QTest::keyClick(control, Qt::Key_Space);
        else QTest::mouseClick(control, Qt::LeftButton, Qt::NoModifier, QPoint(8,control->height()/2));
        QVERIFY(control->isEnabled()); QVERIFY(control->hasFocus());
        QCOMPARE(hint->text(), text); QCOMPARE(hint->geometry(), geometry);
        QTRY_VERIFY(!layer->busy() && layer->onTop() == on);
        QTest::qWait(30);
        QVERIFY(control->hasFocus()); QCOMPARE(control->isChecked(), on);
        QCOMPARE(hint->text(), text); QCOMPARE(hint->geometry(), geometry);
    }
    if (QGuiApplication::platformName().startsWith("wayland")) QVERIFY(pending >= 2);
    QVERIFY(pendingKeptFocus); QVERIFY(pendingKeptLayout);
    QSettings().remove("workspace/alwaysOnTop");
}

void TestSessionsWindow::emptyProjectsSettingPreservesArchiveAndProjects()
{
    SessionOrganization org;
    const auto active = org.createGroup("Active"), empty = org.createGroup("Empty"), archived = org.createGroup("Archived");
    auto state = fleet(); auto box = state.local();
    auto past = box.sessions.first(); past.name = "codex/hgs/old"; past.state = "archived"; past.archiveId = "old-run"; past.runId = "old-run";
    box.sessions.append(past); state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    org.moveSession(org.observe("arch", box.sessions.first().name, box.sessions.first().runId), active);
    org.moveSession(org.observe("arch", past.name, past.runId, past.archiveId), archived);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *list = window.findChild<SessionList *>("sessionList");
    const auto hasHeader = [&](const QString &id) {
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::Header).toBool() && list->item(i)->data(SessionRoles::Group).toString() == id) return true;
        return false;
    };
    QVERIFY(hasHeader(active)); QVERIFY(!hasHeader(empty)); QVERIFY(!hasHeader(archived));
    const auto changeSetting = [&](bool enabled, bool save) {
        window.findChild<QPushButton *>("workspaceSettings")->click();
        QVERIFY(!QApplication::activeModalWidget());auto *page=window.findChild<QWidget *>("settingsPage");QVERIFY(page&&page->isVisible());
        page->findChild<QListWidget *>("settingsSections")->setCurrentRow(1);
        auto *check=page->findChild<QCheckBox *>("workspaceHideEmptyProjects");QVERIFY(check);
        if(save)check->setChecked(enabled);
        window.showSession({},"codex/hgs/dashboard");
    };
    changeSetting(false, false); QVERIFY(!hasHeader(empty));
    changeSetting(false, true); QVERIFY(hasHeader(empty)); QVERIFY(hasHeader(archived));
    QVERIFY(!QSettings().value("workspace/hideEmptyProjects").toBool());
    { SessionsWindow reopened(program()); reopened.setFleet(state); reopened.show();
      auto *rows = reopened.findChild<SessionList *>("sessionList"); bool found = false;
      for (int i = 0; i < rows->count(); ++i) found |= rows->item(i)->data(SessionRoles::Group).toString() == empty;
      QVERIFY(found); }
    changeSetting(true, true); QVERIFY(!hasHeader(empty)); QVERIFY(!hasHeader(archived));
    for (auto *button : window.findChildren<QPushButton *>("sessionFilter")) if (button->property("filter") == "archived") button->click();
    QVERIFY(hasHeader(archived)); QVERIFY(!hasHeader(active));
    window.showSession({}, box.sessions.first().name);
    box.sessions.first().activity = "idle"; box.sessions.first().phase = "idle"; box.sessions.first().state = "paused";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); QVERIFY(hasHeader(active));
    box.sessions.removeFirst(); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); QVERIFY(!hasHeader(active));
    const SessionOrganization stored(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QVERIFY(stored.group(empty)); QVERIFY(stored.group(active)); QVERIFY(stored.group(archived));
}

void TestSessionsWindow::unreadRepliesNeedAnActiveVisibleResult()
{
    auto state = fleet(); auto box = state.local(); auto &session = box.sessions[0];
    session.activity = "idle"; session.phase = "idle"; session.conversationId = "read-conversation";
    session.replyId = "10:100"; session.runId = "read-run"; session.currentTool.clear();
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionOrganization org; const auto group = org.createGroup("Results");
    org.moveSession(org.observe("arch", session.name, session.runId), group); org.setCollapsed(group, true);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    const QString fixture = m_dir->filePath("unread-hgs"); QFile file(fixture); QVERIFY(file.open(QIODevice::WriteOnly));
    const QJsonObject details{{"tracked", true}, {"run_id", session.runId}, {"conversation_id", session.conversationId}, {"reply_id", session.replyId},
        {"activity", "idle"}, {"phase", "idle"}, {"cursor", 10}, {"events", QJsonArray{QJsonObject{{"seq", 10}, {"type", "Stop"}, {"at", 100},
        {"detail", QString("A long response paragraph.\n\n").repeated(90)}}}}};
    file.write("#!/usr/bin/env python3\nimport json\nprint(" + QJsonDocument(QJsonArray{QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact))}).toJson(QJsonDocument::Compact) + "[0])\n");
    file.close(); QVERIFY(FakeCli::makeRunnable(file.fileName()));
    SessionsWindow window(FakeCli::program(fixture)); window.setFleet(state); window.show(); window.findChild<QPushButton *>("brandMark")->click();
    QSignalSpy seen(&window, &SessionsWindow::replyViewed);
    auto *list = window.findChild<SessionList *>("sessionList");
    const auto row = [&]() -> QListWidgetItem * { for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::Identity).toString() == org.observe("arch", session.name, session.runId)) return list->item(i); return nullptr; };
    QVERIFY(row()); QVERIFY(row()->data(SessionRoles::Unread).toBool()); QVERIFY(row()->isHidden());
    for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::Header).toBool() && list->item(i)->data(SessionRoles::Group) == group)
        QCOMPARE(list->item(i)->data(SessionRoles::Unread).toInt(), 1);
    auto *dash = window.findChild<DashboardPage *>(); QVERIFY(dash->findChild<QPushButton *>("dashboardAttention")->text().startsWith("2"));
    window.showSession({}, session.name); window.hide(); QTest::qWait(1650); QCOMPARE(seen.size(), 0);
    window.show(); window.activateWindow(); QApplication::setActiveWindow(&window);
    auto *activity = window.findChild<ActivityView *>("mainActivity"); auto *scroll = activity->browser()->verticalScrollBar();
    QTRY_VERIFY(scroll->maximum() > 0); scroll->setValue(0); QTest::qWait(1650); QCOMPARE(seen.size(), 0);
    // A result loaded behind another app is still unread.
    QWidget other; other.show(); other.activateWindow(); QApplication::setActiveWindow(&other);
    activity->jumpToLatest(); QTest::qWait(1650); QCOMPARE(seen.size(), 0); other.hide();
    window.activateWindow(); QApplication::setActiveWindow(&window);
    for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "attention") button->click();
    QTRY_COMPARE_WITH_TIMEOUT(seen.size(), 1, 3500);
    QCOMPARE(seen[0], QVariantList({QString(), session.name, session.conversationId, session.replyId}));
    QVERIFY(row()); QVERIFY(!row()->data(SessionRoles::Unread).toBool());
    QCOMPARE(list->currentItem()->data(SessionRoles::Identity), row()->data(SessionRoles::Identity));
    QVERIFY(state.markReplyRead({}, session.name, session.conversationId, session.replyId));
    window.setFleet(state); QVERIFY(!row()->data(SessionRoles::Unread).toBool());
    window.hide(); box.sessions[0].replyId = "11:101"; state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    window.show(); window.findChild<QPushButton *>("brandMark")->click();
    QVERIFY(row()->data(SessionRoles::Unread).toBool());
    // A previous unread reply must not hide the next turn's live activity.
    box.sessions[0].activity = "busy"; box.sessions[0].phase = "tool";
    box.sessions[0].activitySummary = "Bash"; box.sessions[0].activityDetail = "Check the next deployment";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    // The hidden list renders when shown; reading the reply takes a visible dwell.
    window.showSessionList();
    QVERIFY(!row()->data(SessionRoles::Unread).toBool()); QVERIFY(row()->data(SessionRoles::Working).toBool());
    QVERIFY(state.local().sessions[0].unreadReply);
    QCOMPARE(row()->data(SessionRoles::Detail).toString(), QString("Bash: Check the next deployment"));
    QVERIFY(window.findChild<QLabel *>("listSummary")->text().contains("1 working"));
    box.sessions[0].activity = "idle"; box.sessions[0].phase = "idle";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(row()->data(SessionRoles::Unread).toBool());
    QCOMPARE(state.local().sessions[0].replyId, QString("11:101"));
}

void TestSessionsWindow::unsentDraftBecomesARowStatus()
{
    // A composer draft marks its row (the delegate turns it into the Draft status).
    auto state = fleet(); const auto box = state.local();
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, box.sessions[0].name);
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    auto *list = window.findChild<SessionList *>("sessionList");
    const auto draft = [&](const QString &name) {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(SessionRoles::Key).toString().endsWith('\n' + name)) return list->item(i)->data(SessionRoles::Draft).toBool();
        return false;
    };
    setComposerText(composer, "Unsent question");
    QVERIFY(draft(box.sessions[0].name));
    window.showSession({}, box.sessions[1].name);
    window.setFleet(state);   // rows are refreshed in place; the draft stays marked
    QVERIFY(draft(box.sessions[0].name)); QVERIFY(!draft(box.sessions[1].name));
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({"@local"});   // other rows go: the list is rebuilt from scratch
    QVERIFY(draft(box.sessions[0].name));
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({});
    window.showSession({}, box.sessions[0].name); composer->editor()->clear();
    QVERIFY(!draft(box.sessions[0].name));
}

void TestSessionsWindow::markAllReadIgnoresFiltersAndKeepsCurrentDraft()
{
    auto state = fleet(); auto box = state.local();
    for (int i : {0, 1, 2}) {
        box.sessions[i].conversationId = QString("bulk-%1").arg(i); box.sessions[i].replyId = "10:100";
    }
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    auto remote = *state.peer("mac"); remote.sessions[0].conversationId = "bulk-remote"; remote.sessions[0].replyId = "20:200";
    state.setPeer(remote, QDateTime::currentMSecsSinceEpoch());
    remote.host = "sleeping"; state.setPeer(remote, 0); remote.ok = false; state.setPeer(remote, 1);
    SessionOrganization org; const auto group = org.createGroup("Collapsed");
    org.moveSession(org.observe("arch", box.sessions[1].name, box.sessions[1].runId), group); org.setCollapsed(group, true);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, box.sessions[0].name);
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({"@local"});
    for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "attention") button->click();
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Keep this draft");
    auto *list = window.findChild<SessionList *>("sessionList"); const auto selectedKey = composer->sessionKey();
    auto *action = window.findChild<QAction *>("markAllSessionsRead"); QVERIFY(action); QVERIFY(action->isEnabled());
    QVERIFY(action->text().contains("(5)"));
    // Marks are shared through each session's host, not kept on this device.
    QCOMPARE(action->toolTip(), QString("Mark current replies across all machines as read, including hidden sessions. "
                                        "Pending approvals, questions and errors stay highlighted."));
    auto *context = window.findChild<QMenu *>("sessionProjectsMenu"); QVERIFY(context); QVERIFY(context->actions().contains(action));
    auto *batch = window.findChild<QPushButton *>("batchActions")->menu(); QMetaObject::invokeMethod(batch, "aboutToShow");
    QVERIFY(batch->actions().contains(action));
    QSignalSpy seen(&window, &SessionsWindow::repliesMarkedRead);
    QSignalSpy writes(window.findChild<HgsClient *>(), &HgsClient::writeDone);
    // Model the root's persisted read state and its next regular fleet update.
    connect(&window, &SessionsWindow::repliesMarkedRead, &window, [&](const QJsonObject &replies) {
        QCOMPARE(state.markRepliesRead(replies), replies.size());
        QSettings().setValue("attention/readReplies", QJsonDocument(state.readReplies()).toJson(QJsonDocument::Compact));
        window.setFleet(state);
    });
    action->trigger(); QCOMPARE(seen.size(), 1); QCOMPARE(seen[0][0].toJsonObject().size(), 5);
    QCOMPARE(window.findChild<QLabel *>("notice")->text(), QString("5 replies marked as read."));
    QVERIFY(!action->isEnabled()); QVERIFY(state.unreadReplies().isEmpty()); QCOMPARE(writes.size(), 0);
    QVERIFY(list->currentItem()); QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(), selectedKey);
    QCOMPARE(composer->sessionKey(), selectedKey); QCOMPARE(composer->editor()->toPlainText(), QString("Keep this draft"));
    // Overview renders when shown.
    window.findChild<QPushButton *>("brandMark")->click();
    QVERIFY(window.findChild<DashboardPage *>()->findChild<QPushButton *>("dashboardAttention")->text().startsWith("1"));
    window.showSessionList(); window.findChild<MachineFilter *>("machineFilterChips")->setSelection({});
    window.findChild<QLineEdit *>("search")->setText("review");
    QCOMPARE(window.findChild<QListWidget *>("searchResults")->count(), 1); // The real approval still needs attention.
    FleetState restarted; restarted.setReadReplies(QJsonDocument::fromJson(QSettings().value("attention/readReplies").toByteArray()).object());
    restarted.setLocal(box, QDateTime::currentMSecsSinceEpoch()); QVERIFY(restarted.unreadReplies().isEmpty());
    box.sessions[0].replyId = "11:101"; state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(action->isEnabled()); QVERIFY(action->text().contains("(1)")); // Even when that session is hidden by search.
    action->trigger(); QCOMPARE(seen.size(), 2); QCOMPARE(seen[1][0].toJsonObject().size(), 1); QVERIFY(!action->isEnabled());
    QCOMPARE(window.findChild<QLabel *>("notice")->text(), QString("1 reply marked as read."));
    QSettings().remove("attention/readReplies");
}

void TestSessionsWindow::sharedReadsQueueOnTheSessionHost()
{
    QSettings().remove("attention/pendingReads");
    auto state = fleet(); auto box = state.local();
    box.sessions[0].runId = "run-local"; box.sessions[0].conversationId = "shared-local";
    box.sessions[0].replyId = "10:100"; box.sessions[0].sharedRead = true;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    auto remote = *state.peer("mac");
    remote.sessions[0].runId = "run-remote"; remote.sessions[0].conversationId = "shared-remote";
    remote.sessions[0].replyId = "20:200"; remote.sessions[0].sharedRead = true;
    state.setPeer(remote, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *action = window.findChild<QAction *>("markAllSessionsRead"); QVERIFY(action); QVERIFY(action->isEnabled());
    action->trigger();
    const auto pending = ReadMarkStore::load();
    const auto local = ReadMark::fromJson(pending.value(ReadMark{{}, box.sessions[0].name, {}, "shared-local", {}, {}}.key()).toObject());
    QCOMPARE(local.runId, QString("run-local")); QCOMPARE(local.replyId, QString("10:100"));
    const auto peer = ReadMark::fromJson(pending.value(ReadMark{"mac", remote.sessions[0].name, {}, "shared-remote", {}, {}}.key()).toObject());
    QCOMPARE(peer.runId, QString("run-remote")); QCOMPARE(peer.replyId, QString("20:200"));
    QSettings().remove("attention/pendingReads");
}

void TestSessionsWindow::compactWorkspaceGeometry()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.resize(960, 650); window.show(); QTest::qWait(30);
    window.showSession({}, "codex/hgs/dashboard");
    const auto *sidebar = window.findChild<QWidget *>("sidebar"); QCOMPARE(sidebar->width(), 56);
    for (const auto *button : sidebar->findChildren<QPushButton *>("railButton")) {
        QVERIFY(button->height() >= 44); QCOMPARE(button->iconSize(), QSize(24, 24));
    }
    const auto *search = window.findChild<QLineEdit *>("search"); const auto *list = window.findChild<QListWidget *>("sessionList");
    QVERIFY(qAbs(search->width() - list->width()) < 8);
    const auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
    QVERIFY2(tabs->mapTo(&window, QPoint()).y() < 100, "Normal session header must leave the workspace near the top");
    QVERIFY(tabs->height() > 500);
    QVERIFY(window.findChild<QAction *>("pauseAction")->toolTip().contains("Pause"));
    QVERIFY(window.findChild<QLabel *>("sessionMeta")->toolTip().contains("gpt-6.1-sol"));
    // The header keeps frequent actions; connect command, pause and resume live in its menu.
    auto *more = window.findChild<QPushButton *>("more"); QList<QPushButton *> row;
    for (auto *button : more->parentWidget()->findChildren<QPushButton *>(Qt::FindDirectChildrenOnly))
        if (button->isVisible() && button->property("sessionHeaderAction").toBool() && qAbs(button->y() - more->y()) < 4) row << button;
    std::sort(row.begin(), row.end(), [](const QPushButton *a, const QPushButton *b) { return a->x() < b->x(); });
    QStringList header; for (const auto *button : row) header << button->objectName();
    QCOMPARE(header, (QStringList{"openSessionFolderAction", "openFolderShellAction", "headerCopySessionIdAction", "clearSessionAction", "more"}));
    QStringList menu; for (const auto *action : more->menu()->actions()) if (!action->isSeparator() && action->isVisible()) menu << action->objectName();
    QCOMPARE(menu, (QStringList{"renameSessionAction", "forkSessionAction", "openSessionAction", "pauseAction", "forgetSessionAction"}));
    QCOMPARE(window.findChild<QAction *>("pauseAction")->text(), QString("Pause session"));
    QVERIFY(!window.findChild<QPushButton *>("restoreSessionAction")->isVisible());
}

void TestSessionsWindow::usageLimitBecomesToolbarChip()
{
    SessionsWindow window(program());window.resize(1100,760);window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<AccountUsageStore *>()->findChild<HgsClient *>();
    auto *chip=window.findChild<ToolbarChip *>("usageLimitChip");QVERIFY(chip);QVERIFY(chip->isHidden());
    const auto now=QDateTime::currentSecsSinceEpoch();
    client->accountsReady(1,{},QJsonObject{{"id","native-codex"},{"status","ok"},{"windows",QJsonArray{
        QJsonObject{{"window_minutes",300},{"used_percent",100},{"resets_at",now+3600}}}}});
    QTRY_VERIFY(chip->isVisible());QVERIFY(chip->fullLabel().startsWith("Limit reached"));QCOMPARE(chip->tone(),ChipTone::Danger);
    QVERIFY(chip->toolTip().contains("Resets in"));
    QTest::mouseClick(chip,Qt::LeftButton);QTRY_VERIFY(chip->popover()->isVisible());
    QVERIFY(window.findChild<QLabel *>("activityUsageWarning")->text().contains("Resets in"));
    auto *refresh=window.findChild<QPushButton *>("usageLimitRefresh");QVERIFY(refresh->isVisible());refresh->click();
    QVERIFY(static_cast<AccountUsage::RefreshButton *>(window.findChild<QPushButton *>("sessionUsageRefresh"))->isRefreshing());
    // Routine re-rendering (refresh, polling inspections) keeps the open popover.
    QVERIFY(chip->popover()->isVisible());
    window.findChild<HgsClient *>()->inspectionReady({},"codex/hgs/dashboard",{{"tracked",true},{"conversation_id","conversation-one"}});
    QVERIFY(chip->popover()->isVisible());
    // Switching session closes the popover rather than showing another account's limit.
    window.showSession("mac","claude/infra/review");QVERIFY(!chip->popover()->isVisible());
}

void TestSessionsWindow::earlyQuotaResetExplainsPendingNativeChoiceAndKeepsDraft()
{
    const auto now=QDateTime::currentSecsSinceEpoch();const QString hash(64,'a');
    const auto quotaWindow=[now](int minutes,qint64 resetIn){return QJsonObject{{"window_minutes",minutes},{"used_percent",0},{"resets_at",double(now+resetIn)}};};
    auto state=fleet();auto box=state.local();auto &session=box.sessions[0];
    session.cmd="claude";session.runId="run-one";session.conversationId="conversation-one";session.accountId="native-claude";state.setLocal(box,0);
    const QJsonObject card{{"source","claude_usage_limit"},{"question_id","claude-usage-limit:"+hash},{"question_hash",hash},
        {"run_id","run-one"},{"conversation_id","conversation-one"},{"can_answer",true},{"answer_transport","claude_tui"},
        {"questions",QJsonArray{QJsonObject{{"id","usage_limit"},{"question","What do you want to do?"},{"header","Usage limit"},
            {"options",QJsonArray{QJsonObject{{"id","limit_0"},{"label","Stop and wait for limit to reset"}},
                QJsonObject{{"id","limit_1"},{"label","Switch to usage credits"}}}}}}}};
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"runtime_state","live"},{"process_state","running"},{"last_event_at",double(now-60)},{"provider_status_at",double(now-60)},
        {"phase","input"},{"activity","busy"},{"attention_id","claude-usage-limit:"+hash},
        {"provider_error",QJsonObject{{"error_kind","quota"},{"at",double(now-60)},{"detail","Weekly limit reached"}}},
        {"pending_questions",QJsonArray{card}},{"events",QJsonArray{}},{"cursor",0}};
    const QJsonObject usage{{"id","native-claude"},{"status","ok"},{"checked_at",double(now)},
        {"windows",QJsonArray{quotaWindow(300,3600),quotaWindow(10080,86400)}}};
    SessionsWindow window(programInspecting("claude-limit-reset",details,{},usage));window.setFleet(state);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer,"Continue after my reset");
    auto *chip=window.findChild<ToolbarChip *>("usageLimitChip");
    QTRY_VERIFY(chip->isVisible());QCOMPARE(chip->fullLabel(),QString("Usage available"));QCOMPARE(chip->tone(),ChipTone::Neutral);
    QVERIFY(chip->toolTip().contains("Answer any open usage-limit choice"));
    auto *question=window.findChild<QuestionCard *>();QVERIFY(question->isVisible());
    const auto options=question->findChildren<QAbstractButton *>("questionOption");QCOMPARE(options.size(),2);
    for(const auto *option:options)QVERIFY(!option->isChecked());
    QVERIFY(!composer->findChild<QPushButton *>("sendMessage")->isEnabled());
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue after my reset"));
}

void TestSessionsWindow::openingChildClearsHiddenAccountUsageRecoveryNotice()
{
    const auto now=QDateTime::currentSecsSinceEpoch();
    auto state=fleet();auto box=state.local();auto &session=box.sessions[0];
    session.cmd="claude";session.runId="run-one";session.conversationId="conversation-one";session.accountId="native-claude";state.setLocal(box,0);
    const QJsonObject children{{"review",QJsonObject{{"name","Review"},{"state","working"}}}};
    const QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"runtime_state","live"},{"process_state","running"},{"last_event_at",double(now-60)},{"provider_status_at",double(now-60)},
        {"phase","error"},{"activity","attention"},{"subagents",children},
        {"provider_error",QJsonObject{{"error_kind","quota"},{"at",double(now-60)},{"detail","Weekly limit reached"}}},
        {"events",QJsonArray{}},{"cursor",0}};
    const QJsonObject child{{"parent_conversation_id","conversation-one"},{"conversation_id","child-conversation"},
        {"run_id","run-one"},{"events",QJsonArray{}}};
    const QJsonObject usage{{"id","native-claude"},{"status","ok"},{"checked_at",double(now)},
        {"windows",QJsonArray{QJsonObject{{"window_minutes",300},{"used_percent",0},{"resets_at",double(now+3600)}},
            QJsonObject{{"window_minutes",10080},{"used_percent",0},{"resets_at",double(now+86400)}}}}};
    SessionsWindow window(programInspecting("quota-child-switch",details,child,usage));window.setFleet(state);window.show();window.showSession({},session.name);
    auto *help=window.findChild<QLabel *>("providerErrorHelp");
    QTRY_VERIFY(help->text().contains("Latest account usage is below the limit"));
    auto *stack=window.findChild<QStackedWidget *>("activityStack");
    window.findChild<QTextBrowser *>("subagents")->anchorClicked(QUrl("hgs-agent:review"));
    QCOMPARE(stack->currentIndex(),1);
    QVERIFY(window.findChild<ToolbarChip *>("usageLimitChip")->isHidden());
    QVERIFY2(!help->text().contains("Latest account usage is below the limit"),qPrintable(help->text()));
    QVERIFY(help->text().contains("will not retry automatically"));
    // Returning projects the same fresh parent-account evidence again.
    window.findChild<QPushButton *>("subagentBack")->click();QCOMPARE(stack->currentIndex(),0);
    QTRY_VERIFY(help->text().contains("Latest account usage is below the limit"));
}

void TestSessionsWindow::accountUsageRejectsOtherSessionReplies()
{
    SessionsWindow window(program());window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<AccountUsageStore *>()->findChild<HgsClient *>();auto *usage=window.findChild<QPushButton *>("sessionAccountUsage");
    const QJsonObject first{{"id","native-codex"},{"identity",QJsonObject{{"email","first@example.test"}}},{"status","ok"}};
    auto *refresh=static_cast<AccountUsage::RefreshButton *>(window.findChild<QPushButton *>("sessionUsageRefresh"));
    QVERIFY(refresh->isRefreshing());QVERIFY(!refresh->isEnabled());
    client->accountsReady(1,{},first);QVERIFY(usage->toolTip().contains("first@example.test"));QVERIFY(!refresh->isRefreshing());QVERIFY(refresh->isEnabled());
    window.showSession("mac","claude/infra/review");QVERIFY(!usage->toolTip().contains("first@example.test"));
    client->accountsReady(1,{},first);QVERIFY(!usage->toolTip().contains("first@example.test"));
    const QJsonObject second{{"id","native-claude"},{"identity",QJsonObject{{"email","second@example.test"}}},{"status","ok"}};
    client->accountsReady(2,"mac",second);QVERIFY(usage->toolTip().contains("second@example.test"));
    refresh->click();QVERIFY(refresh->isRefreshing());QVERIFY(!refresh->isEnabled());QVERIFY(usage->toolTip().contains("second@example.test"));
    client->accountsFailed(3,"mac","offline");QVERIFY(!refresh->isRefreshing());QVERIFY(refresh->isEnabled());QVERIFY(usage->toolTip().contains("second@example.test"));
}

void TestSessionsWindow::usageIndicatorSettingUpdatesFooter()
{
    QSettings().remove("workspace/usageIndicator");
    SessionsWindow window(script()); window.setFleet(fleet());
    auto *usage = static_cast<AccountUsage::Button *>(window.findChild<QPushButton *>("sessionAccountUsage"));
    auto *option = window.findChild<QComboBox *>("workspaceUsageIndicator"); QVERIFY(option);
    QCOMPARE(option->currentData().toString(), QString("rails")); QCOMPARE(usage->indicator(), AccountUsage::Indicator::Rails);
    option->setCurrentIndex(option->findData("marker-countdown"));
    QCOMPARE(QSettings().value("workspace/usageIndicator").toString(), QString("marker-countdown"));
    QCOMPARE(usage->indicator(), AccountUsage::Indicator::MarkerCountdown);
    SessionsWindow restored(script()); restored.setFleet(fleet());
    QCOMPARE(restored.findChild<QComboBox *>("workspaceUsageIndicator")->currentData().toString(), QString("marker-countdown"));
    QCOMPARE(static_cast<AccountUsage::Button *>(restored.findChild<QPushButton *>("sessionAccountUsage"))->indicator(), AccountUsage::Indicator::MarkerCountdown);
    QSettings().remove("workspace/usageIndicator");
}

void TestSessionsWindow::separatesConversationProcessAndTerminal()
{
    SessionInfo session; session.tracked = true; session.state = "running";
    session.activity = "unknown"; session.phase = "unknown";
    session.processState = "running"; session.conversationState = "ended";
    QCOMPARE(SessionsWindow::status(session), QString("Status unknown"));
    session.activity = "idle";
    QCOMPARE(SessionsWindow::status(session), QString("Ready"));
    session.processState = "exited";
    QCOMPARE(SessionsWindow::status(session), QString("Agent exited"));
    session.state = "stopped";
    QCOMPARE(SessionsWindow::status(session), QString("Stopped"));
    session.state = "paused";
    QCOMPARE(SessionsWindow::status(session), QString("Paused"));
    QCOMPARE(SessionsWindow::status(session, false), QString("Offline"));
}

void TestSessionsWindow::attentionNavigationResetsFiltersAndTab()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show();
    auto *search = window.findChild<QLineEdit *>("search"); QVERIFY(search);
    search->setText("does-not-match");
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); QVERIFY(tabs);
    tabs->setCurrentIndex(1);
    window.showProjects("mac");
    window.showAttentionSession("mac", "claude/infra/review");
    QCOMPARE(tabs->currentIndex(), 0); QVERIFY(search->text().isEmpty());
    auto *list = window.findChild<QListWidget *>("sessionList"); QVERIFY(list->currentItem());
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QString("mac\nclaude/infra/review"));
    QVERIFY(list->isVisible());
}

void TestSessionsWindow::backgroundPollKeepsActivityAndComposerStable()
{
    class StyleCounter : public QObject {
    public:
        int changes = 0;
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::StyleChange) ++changes;
            return false;
        }
    } styles;
    SessionsWindow window(program()); window.setFleet(fleet()); window.show();
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspections(client, &HgsClient::inspectionReady);
    window.showSession({}, "codex/hgs/dashboard"); QTRY_VERIFY(!inspections.isEmpty());
    QJsonArray events;
    for (int i = 1; i <= 24; ++i) events.append(QJsonObject{{"seq", i}, {"at", 1791018000 + i},
        {"type", i % 2 ? "UserPromptSubmit" : "Stop"}, {"detail", QString("Persistent message %1\n\nText to keep visible during a background update.").arg(i)}});
    QJsonObject details{{"tracked", true}, {"conversation_id", "conversation-one"}, {"run_id", "run-one"},
        {"activity", "busy"}, {"phase", "working"}, {"last_event_at", 1791018024}, {"cursor", 24}, {"events", events}};
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    auto *activity = window.findChild<ActivityView *>("mainActivity"); QVERIFY(activity); auto *browser = activity->browser();
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    setComposerText(composer, "Unsent draft");
    QTRY_VERIFY(!activity->renderPending());
    auto selection = browser->document()->find("Persistent message 1"); browser->setTextCursor(selection);
    browser->verticalScrollBar()->setValue(0); QTest::qWait(20);
    const auto text = browser->textCursor().selectedText(); QVERIFY(!text.isEmpty());
    const auto geometry = browser->geometry(); const int scroll = browser->verticalScrollBar()->value();
    const int revision = browser->document()->revision();
    window.findChild<QLabel *>("badge")->installEventFilter(&styles);
    composer->findChild<QLabel *>("messageStatus")->installEventFilter(&styles);
    details["events"] = QJsonArray();
    for (int i = 0; i < 4; ++i) {
        window.setConnectionError({}); window.setFleet(fleet());
        details["updated"] = 1791018025 + i; client->inspectionReady({}, "codex/hgs/dashboard", details);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCOMPARE(browser->document()->revision(), revision);
        QCOMPARE(browser->geometry(), geometry); QCOMPARE(browser->verticalScrollBar()->value(), scroll);
        QCOMPARE(browser->textCursor().selectedText(), text); QCOMPARE(composer->editor()->toPlainText(), QString("Unsent draft"));
    }
    QCOMPARE(styles.changes, 0);
    details["cursor"] = 25;
    details["events"] = QJsonArray{QJsonObject{{"seq", 25}, {"at", 1791018025}, {"type", "AgentMessage"}, {"detail", "A new live response"}}};
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    QVERIFY(browser->updatesEnabled()); QTRY_VERIFY(browser->toPlainText().contains("A new live response"));
    QCOMPARE(browser->textCursor().selectedText(), text); QCOMPARE(browser->verticalScrollBar()->value(), scroll);
    QCOMPARE(composer->editor()->toPlainText(), QString("Unsent draft"));
}

void TestSessionsWindow::recoveryEndingReturnsKeyboardFocusToField()
{
    SessionsWindow window(program()); window.resize(1280,900); window.setFleet(fleet()); window.show();
    auto *client=window.findChild<HgsClient *>(); QSignalSpy inspections(client,&HgsClient::inspectionReady);
    window.showSession({},"codex/hgs/dashboard");QTRY_VERIFY(!inspections.isEmpty());
    const auto at=QDateTime::currentMSecsSinceEpoch()/1000.0;
    QJsonObject job{{"id","episode"},{"name","codex/hgs/dashboard"},{"state","waiting"},{"action","continue_message"},
        {"due_at",at+20},{"attempt",0},{"delays",QJsonArray{15,30,60,300}}};
    QJsonObject details{{"tracked",true},{"conversation_id","conversation-one"},{"run_id","run-one"},
        {"activity","attention"},{"phase","error"},{"last_event_at",at},{"cursor",1},{"recovery",job}};
    client->inspectionReady({},"codex/hgs/dashboard",details);
    auto *editor=window.findChild<MessageComposer *>("messageComposer")->editor();
    auto *chip=window.findChild<ToolbarChip *>("recoveryChip");auto *panel=window.findChild<QFrame *>("recoveryPanel");
    window.activateWindow();QVERIFY(QTest::qWaitForWindowActive(&window));
    chip->setFocus(Qt::TabFocusReason);QTRY_VERIFY(chip->hasFocus());
    QTest::keyClick(chip,Qt::Key_Return);QTRY_VERIFY(panel->isVisible());
    auto *now=panel->findChild<QPushButton *>("recoveryNow");now->setFocus();QTRY_VERIFY(now->hasFocus());
    job["state"]="succeeded";details["recovery"]=job;client->inspectionReady({},"codex/hgs/dashboard",details);
    QVERIFY(chip->isHidden());QVERIFY(!panel->isVisible());QTRY_VERIFY(editor->hasFocus());
}

void TestSessionsWindow::recoveryCountdownKeepsHistoryDraftAndFocus()
{
    // Background inspections return the injected state: a poll that started
    // before an injection can finish after it (process start-up is slow on Windows).
    QTemporaryDir temp; QVERIFY(temp.isValid());
    const auto fake=temp.filePath("hgs"), inspected=temp.filePath("inspect.json");
    QVERIFY(FakeCli::replace(inspected,R"({"tracked":true,"conversation_id":"conversation-one","events":[],"cursor":0})"));
    QVERIFY(FakeCli::install(fake,"#!/usr/bin/env python3\nimport runpy,sys\nif 'inspect' in sys.argv[1:3]:sys.stdout.write(open("
        +FakeCli::pythonString(inspected)+").read())\nelse:\n sys.argv[0]="+FakeCli::pythonString(script())+";runpy.run_path(sys.argv[0],run_name='__main__')\n"));
    SessionsWindow window(FakeCli::program(fake)); window.resize(1280,900); window.setFleet(fleet()); window.show();
    auto *client=window.findChild<HgsClient *>(); QSignalSpy inspections(client,&HgsClient::inspectionReady);
    const auto inject=[&](const QJsonObject &details){
        QVERIFY(FakeCli::replace(inspected,QJsonDocument(details).toJson()));client->inspectionReady({},"codex/hgs/dashboard",details);};
    window.showSession({},"codex/hgs/dashboard");QTRY_VERIFY(!inspections.isEmpty());
    const auto at=QDateTime::currentMSecsSinceEpoch()/1000.0;
    QJsonObject job{{"id","episode"},{"name","codex/hgs/dashboard"},{"state","waiting"},{"action","continue_message"},
        {"due_at",at+20},{"attempt",0},{"delays",QJsonArray{15,30,60,300}},
        {"history",QJsonArray{QJsonObject{{"at",at},{"state","waiting"},{"attempt",0}}}}};
    QJsonObject details{{"tracked",true},{"conversation_id","conversation-one"},{"run_id","run-one"},
        {"activity","attention"},{"phase","error"},{"last_event_at",at},{"cursor",1},{"recovery",job},
        {"events",QJsonArray{QJsonObject{{"seq",1},{"at",at},{"type","StopFailure"},{"detail","503 Service unavailable"}}}}};
    inject(details);
    auto *activity=window.findChild<ActivityView *>("mainActivity");auto *browser=activity->browser();
    auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *editor=composer->editor();
    auto *chip=window.findChild<ToolbarChip *>("recoveryChip");QVERIFY(chip && chip->isVisible());QCOMPARE(chip->tone(),ChipTone::Warning);
    QVERIFY(chip->fullLabel().startsWith("Retry in"));QVERIFY(chip->shortLabel().endsWith(" s"));
    auto *panel=window.findChild<QFrame *>("recoveryPanel");QVERIFY(!panel->isVisible());
    setComposerText(composer,"Keep my draft");window.activateWindow();editor->setFocus();QTRY_VERIFY(editor->hasFocus());
    // Account inspection and the initial composer layout are unrelated to retries.
    // Finish them before measuring the effect of the countdown alone.
    auto *usage=static_cast<AccountUsage::RefreshButton *>(window.findChild<QPushButton *>("sessionUsageRefresh"));
    QTRY_VERIFY(!usage->isRefreshing());
    // Deferred startup callbacks can post an ancestor layout after the composer
    // already matches its own size hint. Apply that layout before the baseline.
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
    // Feedback is now a single line, so the layout need not implement height-for-width.
    QTRY_COMPARE(composer->height(),composer->layout()->hasHeightForWidth()
        ? composer->layout()->totalHeightForWidth(composer->width()) : composer->sizeHint().height());
    QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
    QTRY_VERIFY(browser->toPlainText().contains("503 Service unavailable"));
    QTRY_VERIFY(!activity->renderPending());
    QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
    const int revision=browser->document()->revision();const auto geometry=browser->geometry();const auto label=chip->fullLabel();
    QTest::qWait(1100);
    QVERIFY(editor->hasFocus());QCOMPARE(editor->toPlainText(),QString("Keep my draft"));
    QCOMPARE(browser->document()->revision(),revision);QCOMPARE(browser->geometry(),geometry);
    // The panel timer has its own phase: allow one more tick for the next whole second.
    QTRY_VERIFY_WITH_TIMEOUT(chip->fullLabel()!=label,2500);   // the countdown ticks while the popover is closed
    QVERIFY(panel->findChild<QPushButton *>("recoveryNow")->isEnabled());
    const auto preview=qEnvironmentVariable("HGS_RECOVERY_PREVIEW");
    if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/activity.png"));}
    QTest::mouseClick(chip,Qt::LeftButton);QTRY_VERIFY(panel->isVisible());
    if(!preview.isEmpty())QVERIFY(chip->popover()->grab().save(preview+"/recovery-popover.png"));
    QTimer::singleShot(0,&window,[&]{
        auto *dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
        auto *history=dialog->findChild<QPlainTextEdit *>();QVERIFY(history);QVERIFY(history->toPlainText().contains("Retry scheduled"));
        // The dialog belongs to the window, not to the popup it was opened from.
        QCOMPARE(dialog->parentWidget(),static_cast<QWidget *>(&window));QVERIFY(!panel->isVisible());dialog->reject();
    });
    panel->findChild<QPushButton *>("recoveryHistory")->click();
    window.activateWindow();QTest::qWait(20);
    QVERIFY(!panel->isVisible());QTest::mouseClick(chip,Qt::LeftButton);QTRY_VERIFY(panel->isVisible());
    details["events"]=QJsonArray();job["state"]="cancelled";job["reason"]="Cancelled by you";details["recovery"]=job;
    panel->findChild<QPushButton *>("recoveryNow")->setFocus();
    QTRY_VERIFY(panel->findChild<QPushButton *>("recoveryNow")->hasFocus());
    inject(details);
    QVERIFY(panel->hasFocus());QVERIFY(!panel->findChild<QPushButton *>("recoveryNow")->isVisible());
    QCOMPARE(chip->tone(),ChipTone::Warning);QCOMPARE(chip->shortLabel(),QString("Off"));
    job["state"]="succeeded";details["recovery"]=job;inject(details);
    QVERIFY(chip->isHidden());QVERIFY(!panel->isVisible());QTRY_VERIFY(editor->hasFocus());QCOMPARE(editor->toPlainText(),QString("Keep my draft"));
}

void TestSessionsWindow::durableSubmissionKeepsGuiResponsive_data()
{
    QTest::addColumn<QString>("operation"); QTest::addColumn<QString>("outcome");
    for (const QString operation : {"send", "question", "compact", "start"}) {
        for (const QString outcome : {"complete", "run", "conversation", "failure"})
            QTest::addRow("%s-%s", qPrintable(operation), qPrintable(outcome)) << operation << outcome;
        if (operation == "send" || operation == "question") QTest::addRow("%s-expiry", qPrintable(operation)) << operation << QString("expiry");
        if (operation == "compact" || operation == "start") QTest::addRow("%s-cancel", qPrintable(operation)) << operation << QString("cancel");
    }
    QTest::newRow("send-navigation") << QString("send") << QString("navigation");
    QTest::newRow("question-navigation") << QString("question") << QString("navigation");
}

void TestSessionsWindow::durableSubmissionKeepsGuiResponsive()
{
    QFETCH(QString, operation); QFETCH(QString, outcome);
    QTemporaryDir fixtureDir; const QString program = fixtureDir.filePath("hgs");
    QFile fixture(program); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(__file__).parent
a=sys.argv[1:]
if a[0].startswith('@'): a=a[1:]
if a[0]=='inspect':
 print((root/'details.json').read_text() if a[1]=='codex/website/navigation' else '{}')
elif a[0] in ['send','answer','compact-context','resume']:
 p={} if a[0]=='resume' else json.load(sys.stdin)
 with (root/'transport.jsonl').open('a') as output: output.write(json.dumps({'command':a[0],'name':a[1],'payload':p})+'\n')
 if a[0]=='resume': print('{}')
 else:
  result=dict(request_id=p['request_id'],name=a[1],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],status='submitted')
  if a[0]=='answer': result.update(question_id=p['question_id'],question_hash=p['expected_question_hash'])
  print(json.dumps(result))
else: print('{}')
)PY"); fixture.close(); QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    const QString name = "codex/website/navigation";
    auto state = fleet(); auto box = state.local();
    for (auto &session : box.sessions) if (session.name == name) {
        session.runId = "run-one"; session.conversationId = "conversation-one"; session.activity = "idle"; session.phase = "idle"; session.state = "running"; session.processState = "running";
        if (operation == "start") { session.state = "stopped"; session.processState = "exited"; }
    }
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QJsonObject question{{"question_id", "question-one"}, {"question_hash", "hash-one"}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"},
        {"source", "codex_async"}, {"optional", true}, {"can_answer", true}, {"questions", QJsonArray{QJsonObject{{"id", "q_0"},
            {"question", "Which test should run?"}, {"allow_other", true}, {"options", QJsonArray{}}}}}};
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
        {"process_state", "running"}, {"activity", "idle"}, {"phase", "idle"}, {"compact_context_supported", true}, {"events", QJsonArray{}}, {"cursor", 0}};
    if (operation == "start") { details["runtime_state"] = "saved"; details["process_state"] = "exited"; }
    if (operation == "compact") details["cache_hint"] = QJsonObject{{"status", "cold"}};
    if (operation == "question") { details["pending_questions"] = QJsonArray{question}; details["activity"] = "busy"; details["phase"] = "thinking"; }
    const auto save = [&] { return FakeCli::replace(fixtureDir.filePath("details.json"), QJsonDocument(details).toJson()); };
    QVERIFY(save()); SessionsWindow window(FakeCli::program(program)); window.setFleet(state); window.show(); window.showSession({}, name);
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    // A fixture inspection replies with details.json as it was when it started, so one
    // in flight across save() could restore the old identity after the injected one
    // and admit the held submission. HgsClient runs one inspection per session: once
    // ours starts, none is older, and its reply is current.
    const auto settle = [&] { QSignalSpy replies(client, &HgsClient::inspectionReady); qsizetype issued = -1;
        QTRY_VERIFY(issued >= 0 || (client->requestInspection({}, name) && (issued = replies.size(), true)));
        QTRY_VERIFY(std::any_of(replies.cbegin() + issued, replies.cend(), [&](const QList<QVariant> &reply) { return reply.at(1).toString() == name; })); };
    auto *card = window.findChild<QuestionCard *>(); auto *send = composer->findChild<QPushButton *>("sendMessage");
    QSignalSpy inspected(client, &HgsClient::inspectionReady); QTRY_VERIFY(!inspected.isEmpty());
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    QTRY_VERIFY(window.findChild<QLabel *>("detailTitle")->text().contains("navigation"));
    // Receive one actual fixture inspection before replacing its details.
    QTRY_VERIFY(operation == "question" ? card->isVisible() : composer->isInputVisible());
    client->inspectionReady({}, name, details);
    const QString key = composer->sessionKey();
    setComposerText(composer, "Exact preserved message");
    if (operation == "question") { QTRY_VERIFY(!card->isDraftLoading()); card->findChild<QLineEdit *>("questionFreeText")->setText("Exact preserved answer"); }
    ComposerDraftStore::flushWrites(); QCoreApplication::processEvents();
    QSemaphore entered, release; std::atomic<int> writes = 0;
    const QString storageRoot = ComposerDraftStore::directory();
    auto removeBlocker = qScopeGuard([&] { QFile::remove(storageRoot); });
    ComposerDraftStore::setWriteObserver([&] { if (++writes == 1) { entered.release(); release.acquire(); } });
    auto unblock = qScopeGuard([&] { release.release(); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    if (operation == "compact" || operation == "start") QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QMessageBox *>(operation == "compact" ? "coldCacheConfirm" : "startToSendConfirm"); QVERIFY(dialog);
        for (auto *button : dialog->buttons()) if (operation == "compact" ? button->text() == "Compact and continue" : button->objectName() == "resumeAndSend") { button->click(); return; }
        QFAIL("Expected continuation choice");
    });
    auto *submit = operation == "question" ? card->findChild<QPushButton *>("submitQuestionAnswer") : send;
    QTRY_VERIFY(submit->isEnabled());
    // An expired cache at admission opens a modal cold-cache confirmation instead,
    // and nothing here would answer it (zerus-1okk: a Windows shard hung there). Fail
    // instead: the timer can only run once the click returns or a dialog's loop runs.
    bool prompted = false;
    if (outcome == "expiry") {
        // expires_at has whole seconds: +4 leaves at least 3 s for the slow
        // replacement of details.json on Windows and this click.
        const auto deadline = QDateTime::currentSecsSinceEpoch() + 4;
        details["session_usage"] = QJsonObject{{"prompt_cache", QJsonObject{{"status", "warm"}, {"expires_at", deadline}}}};
        QVERIFY(save()); client->inspectionReady({}, name, details);
        QTimer::singleShot(0, &window, [&] {
            for (const auto *prompt : {"coldCacheConfirm", "questionColdCacheConfirm"})
                if (auto *dialog = window.findChild<QMessageBox *>(prompt)) { prompted = true; dialog->reject(); } });
    }
    submit->click(); // Admission captures this authoritative warm TTL without an intervening event turn.
    QVERIFY2(!prompted, "The warm cache expired before admission");
    QTRY_VERIFY(entered.available() == 1);
    if (outcome == "expiry") client->inspectionReady({}, name, details); // Keep latest evidence equal to the captured TTL while persistence is held.
    const auto transports = [&] { QList<QJsonObject> result; QFile file(fixtureDir.filePath("transport.jsonl"));
        if (file.open(QIODevice::ReadOnly)) while (!file.atEnd()) result << QJsonDocument::fromJson(file.readLine()).object(); return result; };
    QVERIFY(transports().isEmpty()); bool heartbeat = false; QTimer::singleShot(0, &window, [&] { heartbeat = true; }); QTRY_VERIFY(heartbeat);
    if (outcome == "navigation") {
        window.showSession({}, "codex/hgs/dashboard");
        heartbeat=false;QTimer::singleShot(0,&window,[&]{heartbeat=true;});QTRY_VERIFY(heartbeat);
        QCOMPARE(composer->sessionKey(), QString("\ncodex/hgs/dashboard"));
    }
    if (outcome == "run" || outcome == "conversation") {
        details[outcome == "run" ? "run_id" : "conversation_id"] = "changed-identity"; QVERIFY(save()); client->inspectionReady({}, name, details); settle();
    }
    if (outcome == "cancel") { auto *cancel = window.findChild<QPushButton *>("cancelCompactSend"); QVERIFY(cancel->isVisible()); cancel->click(); }
    if (outcome == "failure") { QVERIFY(QDir(storageRoot).removeRecursively()); QFile blocker(storageRoot); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close(); }
    if (outcome == "expiry") QTRY_VERIFY_WITH_TIMEOUT(QDateTime::currentSecsSinceEpoch() >= details["session_usage"].toObject()["prompt_cache"].toObject()["expires_at"].toInteger(), 10000);
    QVERIFY(transports().isEmpty()); release.release();
    const bool accepted = outcome == "complete" || outcome == "navigation";
    if (accepted) {
        QTRY_COMPARE(transports().size(), 1); const auto sent = transports().first(); QCOMPARE(sent["name"].toString(), name);
        const QString command = operation == "start" ? "resume" : operation == "compact" ? "compact-context" : operation == "question" ? "answer" : "send";
        QCOMPARE(sent["command"].toString(), command);
        if (operation != "start") { const auto payload = sent["payload"].toObject(); QCOMPARE(payload["expected_run_id"].toString(), QString("run-one"));
            QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-one"));
            if (operation == "send") QCOMPARE(payload["text"].toString(), QString("Exact preserved message"));
            if (operation == "question") QCOMPARE(payload["answers"].toArray().first().toObject()["text"].toString(), QString("Exact preserved answer")); }
    } else {
        if (operation == "question") QTRY_VERIFY(!card->submissionSnapshot(key, "question-one").value("sending").toBool());
        else QTRY_VERIFY(!composer->isSending(key) && !composer->isPreparingSending(key) && !composer->isWaitingToSend(key));
        QVERIFY(transports().isEmpty());
        QCOMPARE(composer->editor()->toPlainText(), QString("Exact preserved message"));
        if (operation == "question" && outcome != "run" && outcome != "conversation") QCOMPARE(card->findChild<QLineEdit *>("questionFreeText")->text(), QString("Exact preserved answer"));
    }
    ComposerDraftStore::flushWrites(); QCoreApplication::processEvents(); QTest::qWait(100);
    QCOMPARE(transports().size(), accepted ? 1 : 0); // Repeated polls never replay the accepted operation.
}

void TestSessionsWindow::coldCacheConfirmationPreservesDraftAndPinsConversation()
{
    const auto program=m_dir->filePath("cache-hgs"),capture=m_dir->filePath("cache-send.json"),inspected=m_dir->filePath("cache-inspect.json");
    // Background inspections return the injected state: an empty reply arriving
    // late would drop the cold cache and leave no confirmation to answer.
    QJsonObject details{{"tracked",true},{"run_id","run-cache"},{"conversation_id","conversation-cache"},{"runtime_state","live"},{"process_state","running"},
        {"activity","idle"},{"phase","idle"},{"cache_hint",QJsonObject{{"status","cold"},{"tokens",756000}}}};
    const auto writeDetails=[&]{QVERIFY(FakeCli::replace(inspected,QJsonDocument(details).toJson()));};
    writeDetails();
    QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport sys,json,pathlib\na=sys.argv[1:]\nif a[0].startswith('@'):a=a[1:]\nif a[0]=='send':\n p=json.load(sys.stdin);target=pathlib.Path("+QJsonDocument(QJsonArray{capture}).toJson(QJsonDocument::Compact)+"[0])\n staged=target.with_name(target.name+'.tmp');staged.write_text(json.dumps(p));staged.replace(target)\n print(json.dumps(dict(status='submitted',request_id=p['request_id'],name=a[1],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'])))\nelif a[0]=='inspect':print(open("+QJsonDocument(QJsonArray{inspected}).toJson(QJsonDocument::Compact)+"[0]).read())\nelse:print('{}')\n");
    fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    // An inspection that read the file before the conversation changes must not be
    // running then. On Windows a 5 ms Qt timer posts its event, so such a reply can be
    // delivered in the same batch right after the confirmation is accepted, before
    // exec() returns: it restores the old conversation, the pinned identity matches
    // again and the draft starts sending. Inspect only on show and selection, then wait
    // for an inspection of the test's own: HgsClient runs one per session, so once its
    // reply is in, none is pending, and the next one starts with the change.
    SessionsWindow window(FakeCli::program(program));window.setFleet(fleet());
    window.setDetailsPollForTests(500,24*60*60*1000);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();QSignalSpy replies(client,&HgsClient::inspectionReady);qsizetype issued=-1;
    // Fixture processes start slowly under load on Windows: the window's inspection
    // may run until HgsClient's 5 s limit before the test's own can start.
    QTRY_VERIFY_WITH_TIMEOUT(issued>=0||(client->requestInspection({},"codex/hgs/dashboard")&&(issued=replies.size(),true)),15000);
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(replies.cbegin()+issued,replies.cend(),[](const QList<QVariant> &reply){return reply.at(1).toString()=="codex/hgs/dashboard";}),15000);
    auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *send=composer->findChild<QPushButton *>("sendMessage");
    setComposerText(composer, "Preserve this draft");QVERIFY(send->isEnabled());const QString key=composer->sessionKey();
    // Act on the confirmation once it is actually shown; a zero timer could fire
    // in another event loop first and find no dialog at all.
    const auto onConfirm=[&](std::function<void(QMessageBox *)> act){
        auto *poll=new QTimer(&window);poll->setInterval(5);QElapsedTimer age;age.start();
        QObject::connect(poll,&QTimer::timeout,&window,[poll,act,age,&window]{
            auto *dialog=window.findChild<QMessageBox *>("coldCacheConfirm");
            if(!(dialog&&dialog->isVisible())&&age.elapsed()<5000)return;
            poll->stop();poll->deleteLater();if(dialog&&dialog->isVisible())act(dialog);});
        poll->start();};
    const auto accept=[](QMessageBox *dialog){for(auto *b:dialog->buttons())if(dialog->buttonRole(b)==QMessageBox::AcceptRole)b->click();};
    bool shown=false;onConfirm([&](QMessageBox *dialog){shown=true;dialog->button(QMessageBox::Cancel)->click();});
    send->click();QVERIFY(shown);QVERIFY(!composer->isSending(key));QVERIFY(!QFileInfo::exists(capture));QCOMPARE(composer->editor()->toPlainText(),QString("Preserve this draft"));
    // The change starts the window's next inspection after the file was replaced. No
    // poll follows, so count the replies after the injected one: that inspection's reply
    // must be seen even if it arrives before the confirmation returns.
    qsizetype injected=-1;
    shown=false;onConfirm([&](QMessageBox *dialog){shown=true;details["conversation_id"]="conversation-new";writeDetails();client->inspectionReady({},"codex/hgs/dashboard",details);injected=replies.size();accept(dialog);});
    send->click();QVERIFY(shown);QVERIFY(!composer->isSending(key));QVERIFY(!QFileInfo::exists(capture));QCOMPARE(composer->editor()->toPlainText(),QString("Preserve this draft"));
    // Wait for that reply, so none is pending when the next confirmation opens.
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(replies.cbegin()+injected,replies.cend(),[](const QList<QVariant> &reply){
        return reply.at(2).toJsonObject().value("conversation_id")==QStringLiteral("conversation-new");}),10000);
    shown=false;onConfirm([&](QMessageBox *dialog){shown=true;accept(dialog);});
    send->click();QVERIFY(shown);QTRY_VERIFY(QFileInfo::exists(capture));QFile sent(capture);QVERIFY(sent.open(QIODevice::ReadOnly));
    const auto payload=QJsonDocument::fromJson(sent.readAll()).object();QCOMPARE(payload["expected_conversation_id"].toString(),QString("conversation-new"));QCOMPARE(payload["text"].toString(),QString("Preserve this draft"));
}

void TestSessionsWindow::clearInspectionUpdatesActivityAndCountersImmediately()
{
    QTemporaryDir temp;const auto program=temp.filePath("hgs");
    QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys
p=pathlib.Path(__file__).parent/'details.json'
print(p.read_text() if 'inspect' in sys.argv and p.exists() else '{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"phase","idle"},{"activity","idle"},{"runtime_state","live"},{"process_state","running"},
        {"conversation_id","old"},{"cursor",40},{"events",QJsonArray{QJsonObject{{"seq",40},{"at",1791018000.0},{"type","Stop"},{"agent_id",""},{"detail","Earlier answer"}}}},
        {"session_usage",QJsonObject{{"status","ok"},{"context",QJsonObject{{"used",109000},{"limit",258400}}}}},
        {"cache_hint",QJsonObject{{"status","cold"},{"tokens",109000}}}};
    // Polls read details.json while it changes: replace it atomically, or a torn read
    // fails an inspection and the session refuses to send.
    const auto writeDetails=[&]{QVERIFY(FakeCli::replace(temp.filePath("details.json"),QJsonDocument(details).toJson()));};
    // The first real inspection must already see the original conversation.
    // An empty startup reply would reset the earlier timeline during draft load.
    writeDetails();
    SessionsWindow window(FakeCli::program(program));window.resize(1080,760);window.setFleet(fleet());window.show();window.showSession("mac","claude/infra/review");
    auto *client=window.findChild<HgsClient *>();
    auto *composer=window.findChild<MessageComposer *>("messageComposer");
    const auto apply=[&]{writeDetails();client->inspectionReady("mac","claude/infra/review",details);};
    settleSelectedInspection(composer,client,"mac","claude/infra/review",details);
    apply();auto *context=window.findChild<QPushButton *>("activityContext");QVERIFY(context->text().contains("42%"));
    auto *cache=window.findChild<ToolbarChip *>("cacheChip");QVERIFY(cache->isActive());
    setComposerText(composer, "Keep this draft");
    auto *activity=window.findChild<ActivityView *>("mainActivity");QVERIFY(activity);
    QTRY_VERIFY(activity->plainText().contains("Earlier answer"));
    settleSelectedInspection(composer,client,"mac","claude/infra/review",details);
    details["conversation_id"]="new";details["session_usage"]=QJsonObject{{"status","unavailable"}};details.remove("cache_hint");
    details["session_clear"]=QJsonObject{{"type","SessionCleared"},{"at",1791018003.0},{"activity_key","clear-new"}};
    details["cleared_conversations"]=QJsonArray{"old"};details["cursor"]=41;
    details["events"]=QJsonArray{QJsonObject{{"seq",41},{"at",1791018004.0},{"type","UserPromptSubmit"},{"agent_id",""},{"detail","Fresh request"}}};
    apply();
    // The first new-conversation reply paints now, before the follow-up read
    // with its reset journal cursor returns.
    QTRY_VERIFY(activity->plainText().contains("Session cleared"));
    // Earlier activity stays in place above the boundary.
    const auto timeline=activity->plainText();
    QVERIFY(timeline.indexOf("Earlier answer")>=0);QVERIFY(timeline.indexOf("Earlier answer")<timeline.indexOf("Session cleared"));
    QVERIFY(timeline.indexOf("Session cleared")<timeline.indexOf("Fresh request"));
    QVERIFY(context->text().isEmpty());QVERIFY(!cache->isActive());QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    details["session_usage"]=QJsonObject{{"status","ok"},{"context",QJsonObject{{"used",32},{"limit",258400}}}};
    apply();QVERIFY(context->text().contains("32/"));QVERIFY(!context->text().contains("109"));
}

void TestSessionsWindow::coldCacheClearKeepsDraftAndPinsIdentity()
{
    QTemporaryDir temp;const auto program=temp.filePath("hgs"),capture=temp.filePath("clear.json");
    QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import sys,json,pathlib,time
root=pathlib.Path(__file__).parent
args=sys.argv[1:]
if 'clear-context' in args:
 p=json.load(sys.stdin);p['args']=args;staged=root/'clear.tmp';staged.write_text(json.dumps(p));staged.replace(root/'clear.json');time.sleep(.2);print(json.dumps(dict(request_id=p['request_id'],name=args[2],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],status='confirmed')))
elif 'inspect' in args: print((root/'details.json').read_text() if (root/'details.json').exists() else '{}')
else: print('{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"phase","idle"},{"activity","idle"},{"runtime_state","live"},{"process_state","running"},
        {"conversation_id","conversation-one"},{"clear_context_supported",true},{"cache_hint",QJsonObject{{"status","cold"},{"tokens",756000}}}};
    // Background inspections read this file at any time: replace it atomically and
    // write it before selecting the session, so a slow first inspection cannot
    // deliver empty details after the test applies its own.
    const auto writeDetails=[&]{QVERIFY(FakeCli::replace(temp.filePath("details.json"),QJsonDocument(details).toJson()));};
    writeDetails();
    SessionsWindow window(FakeCli::program(program));window.resize(1080,760);window.setFleet(fleet());window.show();window.showSession("mac","claude/infra/review");QTest::qWait(150);
    auto *client=window.findChild<HgsClient *>();
    const auto applyDetails=[&]{writeDetails();client->inspectionReady("mac","claude/infra/review",details);};
    // The window inspects on its own, and each reply carries details.json as it was
    // when that inspection started, so one still in flight can bring back identity
    // from before the last change and cancel or allow a confirmation. HgsClient runs
    // one inspection per session: once ours starts none is stale, and its reply is
    // current.
    const auto settle=[&]{QSignalSpy replies(client,&HgsClient::inspectionReady);qsizetype issued=-1;
        QTRY_VERIFY(issued>=0 || (client->requestInspection("mac","claude/infra/review") && (issued=replies.size(),true)));
        QTRY_VERIFY(replies.size()>issued);};
    applyDetails();
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer, "Keep this draft");
    auto *cacheChip=window.findChild<ToolbarChip *>("cacheChip");QVERIFY(cacheChip && cacheChip->isVisible());
    QCOMPARE(cacheChip->fullLabel(),QString("Cold cache"));QCOMPARE(cacheChip->tone(),ChipTone::Danger);
    QTest::mouseClick(cacheChip,Qt::LeftButton);QTRY_VERIFY(cacheChip->popover()->isVisible());
    QVERIFY(window.findChild<QLabel *>("cacheWarning")->text().contains("cold cache"));
    auto *clear=window.findChild<QPushButton *>("cacheClearContext");QVERIFY(clear->isVisible());QVERIFY(clear->isEnabled());QCOMPARE(clear->text(),QString("Clear context"));
    // Every wrapped line fits: the popover is as tall as its content needs at its real width.
    for(auto *label:window.findChild<QWidget *>("cachePopover")->findChildren<QLabel *>())
        QVERIFY2(label->height()>=label->heightForWidth(label->width()),qPrintable(label->objectName()+QString(" %1 < %2").arg(label->height()).arg(label->heightForWidth(label->width()))));
    QSignalSpy launched(&window,&SessionsWindow::newSessionRequested);QSignalSpy finished(client,&HgsClient::sessionActionFinished);
    // A change made while the confirmation is open settles before the answer.
    const auto confirmClear=[&](const std::function<void()> &change={}){QTimer::singleShot(0,&window,[&,change]{
        if(change){change();settle();}
        auto *dialog=window.findChild<QMessageBox *>("clearSessionConfirm");QVERIFY(dialog);
        QCOMPARE(dialog->defaultButton(),dialog->button(QMessageBox::Cancel));
        QCOMPARE(dialog->escapeButton(),dialog->button(QMessageBox::Cancel));
        QVERIFY(dialog->text().contains("claude/infra/review"));QVERIFY(dialog->text().contains("mac"));QVERIFY(dialog->text().contains("draft"));
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();return;}QFAIL("Missing clear confirmation");
    });};
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("clearSessionConfirm");QVERIFY(dialog);dialog->button(QMessageBox::Cancel)->click();});
    clear->click();QVERIFY(!QFileInfo::exists(capture));QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    confirmClear([&]{details["conversation_id"]="changed";applyDetails();});
    clear->click();QVERIFY(!QFileInfo::exists(capture));details["conversation_id"]="conversation-one";applyDetails();settle();
    confirmClear();clear->click();QVERIFY(!clear->isEnabled());QTRY_VERIFY(QFileInfo::exists(capture));
    QFile sent(capture);QVERIFY(sent.open(QIODevice::ReadOnly));const auto payload=QJsonDocument::fromJson(sent.readAll()).object();sent.close();
    QCOMPARE(payload["expected_run_id"].toString(),QString("run-one"));QCOMPARE(payload["expected_conversation_id"].toString(),QString("conversation-one"));
    QCOMPARE(payload["args"].toArray(),QJsonArray({"@mac","clear-context","claude/infra/review","--json"}));
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));QVERIFY(!window.findChild<NewSessionDialog *>());QCOMPARE(launched.size(),0);
    QTRY_COMPARE(finished.size(),1);QVERIFY(finished[0][1].toBool());QVERIFY(QFile::remove(capture));applyDetails();
    // The send warning offers the same reset, without sending the draft.
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("coldCacheConfirm");QVERIFY(dialog);QVERIFY(dialog->text().contains("token usage"));
        for(auto *button:dialog->buttons())if(button->text()=="Clear context"){confirmClear();button->click();return;}QFAIL("Missing clear option");});
    composer->findChild<QPushButton *>("sendMessage")->click();QTRY_VERIFY(QFileInfo::exists(capture));QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    QTRY_COMPARE(finished.size(),2);QVERIFY(QFile::remove(capture));
    // The permanent Details action works with Terminal selected and a warm cache.
    details.remove("cache_hint");applyDetails();
    window.findChild<QPushButton *>("toggleInspector")->setChecked(true);
    auto *inspector=window.findChild<QTabWidget *>("sessionInspector");inspector->setCurrentIndex(1);
    auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");tabs->setCurrentIndex(1);QTest::qWait(30);
    auto *detailsClear=window.findChild<QPushButton *>("clearSessionFromDetails");QVERIFY(detailsClear->isVisible());QVERIFY(detailsClear->isEnabled());
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("clearSessionConfirm");QVERIFY(dialog);dialog->button(QMessageBox::Cancel)->click();});
    detailsClear->click();QVERIFY(!QFileInfo::exists(capture));QCOMPARE(tabs->currentIndex(),1);
    confirmClear([&]{details["run_id"]="new-run";applyDetails();});
    detailsClear->click();QVERIFY(!QFileInfo::exists(capture));settle();
    confirmClear();detailsClear->click();QTRY_VERIFY(QFileInfo::exists(capture));QCOMPARE(tabs->currentIndex(),1);
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));QCOMPARE(launched.size(),0);
    QTRY_COMPARE(finished.size(),3);QVERIFY(QFile::remove(capture));
    window.findChild<QPushButton *>("toggleInspector")->setChecked(false);
    // The header icon exposes the same confirmed reset with the inspector hidden.
    auto *more=window.findChild<QPushButton *>("more");QVERIFY(more);QVERIFY(more->menu());
    auto *headerClear=window.findChild<QPushButton *>("clearSessionAction");QVERIFY(headerClear);
    QVERIFY(headerClear->isVisible());QVERIFY(headerClear->isEnabled());QVERIFY(!more->menu()->findChild<QAction *>("clearSessionAction"));
    QCOMPARE(headerClear->accessibleName(),QString("Clear session"));
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("clearSessionConfirm");QVERIFY(dialog);dialog->button(QMessageBox::Cancel)->click();});
    headerClear->click();QVERIFY(!QFileInfo::exists(capture));QCOMPARE(tabs->currentIndex(),1);
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    details["clear_context_supported"]=false;applyDetails();QVERIFY(!headerClear->isEnabled());QVERIFY(!detailsClear->isEnabled());
    details["clear_context_supported"]=true;applyDetails();QVERIFY(headerClear->isEnabled());
    details.remove("conversation_id");applyDetails();QTRY_VERIFY(!headerClear->isEnabled());
    details["conversation_id"]="conversation-one";applyDetails();QTRY_VERIFY(headerClear->isEnabled());settle();
    confirmClear();headerClear->click();QVERIFY(!headerClear->isEnabled());
    QTRY_VERIFY(QFileInfo::exists(capture));QVERIFY(sent.open(QIODevice::ReadOnly));
    const auto menuPayload=QJsonDocument::fromJson(sent.readAll()).object();sent.close();
    QCOMPARE(menuPayload["expected_run_id"].toString(),QString("new-run"));
    QCOMPARE(menuPayload["expected_conversation_id"].toString(),QString("conversation-one"));
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));QCOMPARE(tabs->currentIndex(),1);
    QTRY_COMPARE(finished.size(),4);QVERIFY(!menuPayload.contains("dismiss_prompt"));QVERIFY(QFile::remove(capture));
    // While Claude shows a chooser, an explicit reset first dismisses it with Esc.
    details["phase"]="input";details["activity"]="busy";details["clear_context_supported"]=false;
    details["clear_context_dismiss"]=true;applyDetails();QTRY_VERIFY(headerClear->isEnabled());settle();
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("clearSessionConfirm");QVERIFY(dialog);
        QCOMPARE(dialog->defaultButton(),dialog->button(QMessageBox::Cancel));
        QVERIFY(dialog->text().contains("Esc"));QVERIFY(dialog->text().contains("denied"));QVERIFY(dialog->text().contains("draft"));
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){
            QCOMPARE(button->text(),QString("Dismiss and clear"));button->click();return;}
        QFAIL("Missing dismissal confirmation");});
    headerClear->click();QTRY_VERIFY(QFileInfo::exists(capture));QVERIFY(sent.open(QIODevice::ReadOnly));
    const auto dismissPayload=QJsonDocument::fromJson(sent.readAll()).object();sent.close();
    QCOMPARE(dismissPayload["dismiss_prompt"].toBool(),true);QCOMPARE(dismissPayload["expected_run_id"].toString(),QString("new-run"));
    QTRY_COMPARE(finished.size(),5);QVERIFY(QFile::remove(capture));
    // A chooser that closed before confirmation is not dismissed.
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("clearSessionConfirm");QVERIFY(dialog);
        details["clear_context_dismiss"]=false;details["clear_context_supported"]=true;details["phase"]="idle";details["activity"]="idle";
        applyDetails();settle();
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();return;}});
    headerClear->click();QTest::qWait(100);QVERIFY(!QFileInfo::exists(capture));
    details["clear_context_supported"]=false;applyDetails();QVERIFY(!headerClear->isEnabled());tabs->setCurrentIndex(0);
    QTest::qWait(350);details["phase"]="compacting";details["activity"]="busy";details["clear_context_supported"]=false;details.remove("cache_hint");details["session_usage"]=QJsonObject{{"status","ok"},{"context",QJsonObject{{"used",215900},{"limit",258400}}}};
    applyDetails();settle();QTest::qWait(50);QVERIFY(!headerClear->isEnabled());
    auto *progress=window.findChild<ActivityView *>("mainActivity")->compactionIndicator();auto *context=window.findChild<QWidget *>("activityContext");
    QVERIFY(progress->isVisible());QVERIFY(qAbs(progress->mapToGlobal(progress->rect().center()).y()-context->mapToGlobal(context->rect().center()).y())<=2);
    const auto preview=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/compaction-footer.png"));}
}

void TestSessionsWindow::coldCacheExplainsUnavailableContextActions()
{
    QTemporaryDir temp;const auto program=temp.filePath("hgs");
    QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import pathlib,sys
p=pathlib.Path(__file__).parent/'details.json'
print(p.read_text() if 'inspect' in sys.argv and p.exists() else '{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    // The main turn finished, but a workflow child still works: the host names it.
    const QString working=QString::fromUtf8("Wait until the agent reports Ready. Still working: subagent “<b>workflow-subagent</b>” running Bash");
    // What a tooltip displays: Qt renders a tooltip that looks like HTML as rich text.
    const auto shownTip=[](const QString &tip){if(!Qt::mightBeRichText(tip))return tip;QTextDocument document;document.setHtml(tip);return document.toPlainText();};
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},{"runtime_state","live"},{"process_state","running"},
        {"activity","busy"},{"phase","working"},{"compact_context_supported",false},{"clear_context_supported",false},
        {"compact_context_reason",working},{"clear_context_reason",working},{"cache_hint",QJsonObject{{"status","cold"},{"tokens",756000}}}};
    // Inspections read details.json at any time: replace it atomically and write it
    // before selecting the session.
    const auto writeDetails=[&]{QVERIFY(FakeCli::replace(temp.filePath("details.json"),QJsonDocument(details).toJson()));};
    writeDetails();
    SessionsWindow window(FakeCli::program(program));keepDetailsPollsOut(window);window.resize(1080,760);window.setFleet(fleet());
    window.show();window.showSession("mac","claude/infra/review");
    auto *client=window.findChild<HgsClient *>();auto *composer=window.findChild<MessageComposer *>("messageComposer");
    const auto apply=[&]{writeDetails();settleSelectedInspection(composer,client,"mac","claude/infra/review",details);};
    apply();setComposerText(composer,"Keep this draft");const auto key=composer->sessionKey();
    auto *send=composer->findChild<QPushButton *>("sendMessage");QVERIFY(send->isEnabled());
    // Opens the send confirmation once it is shown, records its context actions and cancels.
    struct Shown{bool seen=false,compact=false,clear=false,plain=false;QString compactTip,clearTip,line;} shown;
    const auto confirm=[&]{
        shown={};auto *poll=new QTimer(&window);poll->setInterval(5);QElapsedTimer age;age.start();
        QObject::connect(poll,&QTimer::timeout,&window,[poll,age,&window,&shown,shownTip]{
            auto *dialog=window.findChild<QMessageBox *>("coldCacheConfirm");
            if(!(dialog&&dialog->isVisible())&&age.elapsed()<5000)return;
            poll->stop();poll->deleteLater();if(!dialog||!dialog->isVisible())return;
            shown.seen=true;shown.line=dialog->informativeText();
            // The reported names stay literal text.
            const auto labels=dialog->findChildren<QLabel *>();
            shown.plain=std::any_of(labels.cbegin(),labels.cend(),[&](const QLabel *label){return label->text()==shown.line;})
                && std::all_of(labels.cbegin(),labels.cend(),[&](const QLabel *label){
                    return label->text()!=shown.line||label->textFormat()==Qt::PlainText;});
            for(auto *button:dialog->buttons()){
                if(button->text()=="Compact and continue"){shown.compact=button->isEnabled();shown.compactTip=shownTip(button->toolTip());}
                if(button->text()=="Clear context"){shown.clear=button->isEnabled();shown.clearTip=shownTip(button->toolTip());}
            }
            dialog->button(QMessageBox::Cancel)->click();});
        poll->start();send->click();};
    confirm();QVERIFY(shown.seen);QVERIFY(!shown.compact);QVERIFY(!shown.clear);QVERIFY(shown.plain);
    QCOMPARE(shown.compactTip,working+'.');QCOMPARE(shown.clearTip,working+'.');
    QCOMPARE(shown.line,"Compact and continue and Clear context are unavailable. "+working+'.');
    QVERIFY(!composer->isSending(key));QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    // The persistent clear controls carry the same reason.
    auto *cacheClear=window.findChild<QPushButton *>("cacheClearContext");auto *detailsClear=window.findChild<QPushButton *>("clearSessionFromDetails");
    auto *headerClear=window.findChild<QPushButton *>("clearSessionAction");QVERIFY(headerClear);
    QVERIFY(!cacheClear->isEnabled());QVERIFY(!detailsClear->isEnabled());QVERIFY(!headerClear->isEnabled());
    QCOMPARE(shownTip(cacheClear->toolTip()),working+'.');QCOMPARE(shownTip(detailsClear->toolTip()),working+'.');QCOMPARE(shownTip(headerClear->toolTip()),working+'.');
    // An offline host's last report no longer explains the actions.
    auto offline=fleet();auto peer=*offline.peer("mac");peer.ok=false;offline.setPeer(peer,QDateTime::currentMSecsSinceEpoch());
    window.setFleet(offline);QVERIFY(!cacheClear->isEnabled());QVERIFY(!detailsClear->isEnabled());
    QCOMPARE(cacheClear->toolTip(),QString::fromUtf8("Clear this agent’s conversation, like /clear in Terminal. Your unsent message stays here."));
    QCOMPARE(detailsClear->toolTip(),QString("Start this agent's conversation from scratch, like /clear in Terminal. Your unsent draft is kept."));
    QCOMPARE(headerClear->toolTip(),"Clear session\n"+detailsClear->toolTip());
    window.setFleet(fleet());apply();QCOMPARE(shownTip(cacheClear->toolTip()),working+'.');QCOMPARE(shownTip(detailsClear->toolTip()),working+'.');
    // A Claude chooser still allows Dismiss and clear, so that action keeps its usual text.
    details["clear_context_dismiss"]=true;apply();
    QVERIFY(detailsClear->isEnabled());QVERIFY(!cacheClear->isEnabled());QCOMPARE(shownTip(cacheClear->toolTip()),working+'.');
    const QString usualClear="Start this agent's conversation from scratch, like /clear in Terminal. Your unsent draft is kept.";
    QCOMPARE(detailsClear->toolTip(),usualClear);QCOMPARE(headerClear->toolTip(),"Clear session\n"+usualClear);
    details.remove("clear_context_dismiss");
    // Each action names its own reason; an available one keeps its usual text.
    const QString resuming="The agent has not confirmed the resumed conversation yet";
    details["compact_context_reason"]=resuming;details["clear_context_supported"]=true;details["clear_context_reason"]="";apply();
    confirm();QVERIFY(shown.seen);QVERIFY(!shown.compact);QVERIFY(shown.clear);
    QCOMPARE(shown.compactTip,resuming+'.');QVERIFY(shown.clearTip.isEmpty());
    QCOMPARE(shown.line,"Compact and continue is unavailable. "+resuming+'.');
    const QString usualCache=QString::fromUtf8("Clear this agent’s conversation, like /clear in Terminal. Your unsent message stays here.");
    QVERIFY(cacheClear->isEnabled());QCOMPARE(cacheClear->toolTip(),usualCache);QCOMPARE(detailsClear->toolTip(),usualClear);
    const QString unconfirmed="The last compaction was not confirmed. Check Terminal.";
    details["compact_context_supported"]=true;details["compact_context_reason"]="";
    details["clear_context_supported"]=false;details["clear_context_reason"]=unconfirmed;apply();
    confirm();QVERIFY(shown.seen);QVERIFY(shown.compact);QVERIFY(!shown.clear);
    QCOMPARE(shown.compactTip,QString("Run native /compact, then send this draft only after successful completion."));
    QCOMPARE(shown.clearTip,unconfirmed);QCOMPARE(shown.line,"Clear context is unavailable. "+unconfirmed);
    // An older host reports no reason: the actions stay disabled without one.
    details["compact_context_supported"]=false;details.remove("compact_context_reason");details.remove("clear_context_reason");apply();
    confirm();QVERIFY(shown.seen);QVERIFY(!shown.compact);QVERIFY(!shown.clear);QVERIFY(shown.line.isEmpty());
    QCOMPARE(shown.compactTip,QString("Run native /compact, then send this draft only after successful completion."));QVERIFY(shown.clearTip.isEmpty());
    QCOMPARE(cacheClear->toolTip(),usualCache);QCOMPARE(detailsClear->toolTip(),usualClear);QCOMPARE(headerClear->toolTip(),"Clear session\n"+usualClear);
    QVERIFY(!composer->isSending(key));QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
}

void TestSessionsWindow::processPollingFollowsVisibilityAndSettings()
{
    QTemporaryDir temp;const auto program=temp.filePath("hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import sys,json,pathlib
root=pathlib.Path(__file__).parent
a=sys.argv[1:]
if a and a[0].startswith('@'):a=a[1:]
if a and a[0]=='inspect':
 with (root/'inspections.jsonl').open('a') as f:f.write(json.dumps(a)+'\n')
 value=dict(tracked=True,run_id='run',conversation_id='conversation',activity='idle',phase='idle',events=[],cursor=0)
 if '--skip-processes' not in a:value['processes']=dict(active_count=1,items=[dict(id='job',command='sleep 300',status='running',owner='main',capabilities=dict(output=False,stop=False))])
 print(json.dumps(value))
else:print('{}')
# Last, once logged, and only for requests (the Terminal tab's attach does not
# go through ProcessRunner).
if a[:1]!=['a']:
 with (root/'done').open('a') as f:f.write('.\n')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    // Requests launched just before a change may still be starting (slowly on
    // Windows): settle() waits until every launched request has logged, so only
    // requests launched afterwards count.
    std::atomic<int> launches=0;ProcessRunner::setLaunchObserver([&launches]{++launches;});
    const auto observer=qScopeGuard([]{ProcessRunner::setLaunchObserver({});ProcessSettings::setPollFloorForTests(0);});
    const auto done=[&]{QFile file(temp.filePath("done"));return file.open(QIODevice::ReadOnly)?int(file.readAll().count('\n')):0;};
    const auto settle=[&]{QTest::qWait(200);const int launched=launches;QTRY_VERIFY_WITH_TIMEOUT(done()>=launched,10000);};
    SessionsWindow window(FakeCli::program(program));window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    // Shorter polls in the production order, on a 100 ms tick: Processes every 0.2 s
    // (set below), Activity every 1 s, Terminal every 5 s. A wait for a poll that must
    // not happen covers an Activity interval and process start-up; polls that must
    // happen are awaited without a tight limit, however slowly processes start.
    constexpr int activityMs=1000;window.setDetailsPollForTests(100,activityMs);
    auto log=[&]{QList<QJsonArray> result;QFile file(temp.filePath("inspections.jsonl"));if(file.open(QIODevice::ReadOnly))while(!file.atEnd())result<<QJsonDocument::fromJson(file.readLine()).array();return result;};
    auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");QVERIFY(tabs);
    QTRY_VERIFY(!log().isEmpty());QTest::qWait(200);
    QVERIFY(!tabs->isTabVisible(3));for(const auto &call:log())QVERIFY(call.contains("--skip-processes"));
    auto *settings=window.findChild<QWidget *>("settingsPage");
    settings->findChild<QDoubleSpinBox *>("activeSeconds")->setValue(1);
    settings->findChild<QDoubleSpinBox *>("backgroundSeconds")->setValue(5);
    QCOMPARE(QSettings().value("processes/activeSeconds").toDouble(),1.);
    ProcessSettings::setPollFloorForTests(0.2);QSettings().setValue("processes/activeSeconds",0.2);
    const auto before=log().size();QTRY_VERIFY(log().size()>before);
    QVERIFY(log().last().contains("--skip-processes"));QCOMPARE(tabs->tabText(3),QString("Processes"));
    auto *enabled=settings->findChild<QCheckBox *>("processesEnabled");QVERIFY(enabled);enabled->setChecked(true);QVERIFY(tabs->isTabVisible(3));
    // Only the Processes interval can poll the open tab again within 3 s: Activity
    // now waits a minute and Terminal 5 s.
    window.setDetailsPollForTests(100,60000);
    tabs->setCurrentIndex(3);QTRY_VERIFY(!log().last().contains("--skip-processes"));
    const auto active=log().size();QTRY_VERIFY_WITH_TIMEOUT(log().size()>active,3000);QVERIFY(!log().last().contains("--skip-processes"));
    window.setDetailsPollForTests(100,activityMs);
    tabs->setCurrentIndex(1);settle();const auto terminal=log().size();QTest::qWait(activityMs+400);QCOMPARE(log().size(),terminal);
    for(const auto &call:log())QCOMPARE(call[1].toString(),QString("codex/hgs/dashboard"));
    tabs->setCurrentIndex(0);QTRY_VERIFY(log().size()>terminal); // Activity refreshes immediately on return.
    // Every Activity poll after settle() is at least the background interval, here
    // 0.2 s, after the last Processes poll: it would include processes if enabled.
    QSettings().setValue("processes/backgroundSeconds",0.2);
    tabs->setCurrentIndex(3);QTest::qWait(200);enabled->setChecked(false);QVERIFY(!tabs->isTabVisible(3));QCOMPARE(tabs->currentIndex(),0);
    settle();const auto disabled=log().size();QTRY_VERIFY(log().size()>disabled);
    for(const auto &call:log().sliced(disabled))QVERIFY(call.contains("--skip-processes"));
    window.hide();settle();const auto hidden=log().size();QTest::qWait(activityMs+400);QCOMPARE(log().size(),hidden);
}

void TestSessionsWindow::localStartsAskAboutAnotherWindowsSession()
{
    // Spec D5: resuming, opening or forking a saved session of this Windows computer
    // asks first when its session host runs in another Windows session.
    const auto reset = qScopeGuard([] { LocalSessionHost::resetOwnWindowsSessionForTests(); });
    LocalSessionHost::setOwnWindowsSessionForTests(1);
    const QString name = "codex/website/navigation";
    auto state = fleet(); auto box = state.local(); box.metrics["platform"] = "windows";
    box.localHost = QJsonObject{{"state", "running"}, {"pid", 4242}, {"protocol", 1}, {"conpty", "bundled"},
        {"integrity", "medium"}, {"windows_session", 2}};
    for (auto &session : box.sessions) if (session.name == name) session.runId = "saved-run";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    const QJsonObject details{{"tracked", true}, {"run_id", "saved-run"}, {"conversation_id", "conversation-one"},
        {"fork_supported", true}, {"runtime_state", "saved"}, {"process_state", "exited"},
        {"events", QJsonArray{}}, {"cursor", 0}};
    const auto fixture = programInspecting("other-windows-session-hgs", details); QVERIFY(!fixture.isEmpty());
    SessionsWindow window(fixture); window.setFleet(state); window.show(); window.showSession({}, name);
    auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
    QSignalSpy opened(&window, &SessionsWindow::sessionActivated);
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Keep this draft");
    auto *resume = window.findChild<QAction *>("pauseAction"); QTRY_VERIFY(resume->isEnabled());
    LaunchWarnings warnings(window);
    // Cancel starts nothing and keeps the draft.
    resume->trigger(); QCOMPARE(warnings.shown, 1);
    QVERIFY(warnings.texts[0].contains("another Windows session (2)"));
    QVERIFY(resume->isEnabled()); QCOMPARE(composer->editor()->toPlainText(), QString("Keep this draft"));
    auto *open = window.findChild<QAction *>("openSessionAction"); QVERIFY(open->isEnabled());
    open->trigger(); QCOMPARE(warnings.shown, 2); QCOMPARE(opened.size(), 0);
    QTest::qWait(200); QCOMPARE(writes.size(), 0);
    // Launch anyway resumes exactly once.
    warnings.answer = "launchAnyway"; resume->trigger(); QCOMPARE(warnings.shown, 3);
    QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 1, 15000); QCOMPARE(writes[0][0].toString(), "resume " + name);
    QTRY_VERIFY(open->isEnabled()); open->trigger(); QCOMPARE(warnings.shown, 4); QCOMPARE(opened.size(), 1);
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep this draft"));
    // A host in the GUI's own Windows session asks nothing.
    LocalSessionHost::setOwnWindowsSessionForTests(2);
    QTRY_VERIFY(resume->isEnabled()); resume->trigger(); QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 2, 15000); QCOMPARE(warnings.shown, 4);
    // Cancel in the warning keeps the fork dialog and its name; Launch anyway forks once.
    LocalSessionHost::setOwnWindowsSessionForTests(1); warnings.answer = "cancel";
    // Fork support arrives with the first inspection, which a loaded runner may delay.
    auto *fork = window.findChild<QAction *>("forkSessionAction"); QTRY_VERIFY_WITH_TIMEOUT(fork->isEnabled(), 20000);
    bool checked = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("forkSessionDialog"); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        auto *forkName = dialog->findChild<QLineEdit *>("forkSessionName"); forkName->setText("forked-here");
        auto *create = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        create->click(); QCOMPARE(warnings.shown, 5);
        QVERIFY(dialog->isVisible()); QCOMPARE(forkName->text(), QString("forked-here")); QCOMPARE(writes.size(), 2);
        warnings.answer = "launchAnyway"; create->click(); QCOMPARE(warnings.shown, 6); checked = true;
    });
    fork->trigger(); QVERIFY(checked);
    QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 3, 15000);
    QVERIFY(writes[2][2].toString().startsWith("fork\n" + name + "\n-n\nforked-here\n-d"));
    QTest::qWait(200); QCOMPARE(writes.size(), 3); QCOMPARE(warnings.shown, 6);
}

void TestSessionsWindow::stoppedSessionStartsFreshOrSendsAfterStart()
{
    QTemporaryDir temp;const auto program=temp.filePath("hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import sys,json,pathlib
root=pathlib.Path(__file__).parent
a=sys.argv[1:]
if a[0].startswith('@'):a=a[1:]
if a[0]=='resume':
 with open(root/'resume.log','a') as log:log.write(json.dumps(a)+'\n')
elif a[0] in ['send','compact-context']:
 p=json.load(sys.stdin);staged=root/(a[0]+'.tmp');staged.write_text(json.dumps(p));staged.replace(root/(a[0]+'.json'))
 print(json.dumps(dict(request_id=p['request_id'],name=a[1],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],first_message=not p['expected_conversation_id'],status='submitted')))
elif a[0]=='inspect':print((root/'details.json').read_text() if a[1]=='codex/website/navigation' and (root/'details.json').exists() else '{}')
else:print('{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    const QString name="codex/website/navigation";
    auto state=fleet();auto box=state.local();
    const auto setState=[&](const QString &value,const QString &run,const QString &conversation){
        for(auto &session:box.sessions)if(session.name==name){session.state=value;session.runId=run;session.conversationId=conversation;
            session.processState=value=="running"?"running":"exited";session.activity="idle";session.phase="idle";}
        state.setLocal(box,QDateTime::currentMSecsSinceEpoch());};
    setState("stopped","run-old","conversation-old");
    QJsonObject details{{"tracked",true},{"run_id","run-old"},{"conversation_id","conversation-old"},{"runtime_state","saved"},{"process_state","exited"}};
    {QFile initial(temp.filePath("details.json"));QVERIFY(initial.open(QIODevice::WriteOnly));initial.write(QJsonDocument(details).toJson());}
    SessionsWindow window(FakeCli::program(program));window.setFleet(state);window.show();window.showSession({},name);QTest::qWait(100);
    auto *client=window.findChild<HgsClient *>();QSignalSpy writes(client,&HgsClient::writeDone);
    // The window polls a starting or compacting session on its own, and each reply
    // carries details.json as it was when that inspection started. HgsClient runs one
    // inspection per session: once ours starts after the write, a stale reply cannot
    // arrive after the new details and cancel the continuation as another run.
    const auto settle=[&]{QSignalSpy replies(client,&HgsClient::inspectionReady);qsizetype issued=-1;
        const auto ours=[&]{qsizetype count=0;for(const auto &reply:replies)count+=reply.at(1).toString()==name;return count;};
        QTRY_VERIFY(issued>=0 || (client->requestInspection({},name) && (issued=ours(),true)));
        QTRY_VERIFY(ours()>issued);};
    // The test and the fixture both replace files atomically: a polled file is never read empty.
    const auto apply=[&]{QVERIFY(FakeCli::replace(temp.filePath("details.json"),QJsonDocument(details).toJson()));
        settle();client->inspectionReady({},name,details);};apply();
    const auto resumes=[&]{QFile log(temp.filePath("resume.log"));return log.open(QIODevice::ReadOnly)?QString::fromUtf8(log.readAll()).split('\n',Qt::SkipEmptyParts):QStringList();};
    const QString freshArgs=R"(["resume", "codex/website/navigation", "--fresh", "--expected-run-id", "run-old", "-d"])";
    // A stopped agent cannot clear natively; the header menu offers a confirmed fresh start.
    auto *fresh=window.findChild<QAction *>("freshSessionAction");QVERIFY(fresh);QVERIFY(fresh->isVisible());QVERIFY(fresh->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>("clearSessionAction")->isVisible());
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("startFreshConfirm");QVERIFY(dialog);
        QCOMPARE(dialog->defaultButton(),dialog->button(QMessageBox::Cancel));QVERIFY(dialog->text().contains(name));
        dialog->button(QMessageBox::Cancel)->click();});
    fresh->trigger();QTest::qWait(100);QVERIFY(resumes().isEmpty());
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("startFreshConfirm");QVERIFY(dialog);
        for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();return;}QFAIL("Missing fresh confirmation");});
    fresh->trigger();QTRY_COMPARE(writes.size(),1);QVERIFY(writes[0][1].toBool());QCOMPARE(resumes(),QStringList{freshArgs});
    // Send stays available and asks how to start; cancelling keeps the draft.
    auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *send=composer->findChild<QPushButton *>("sendMessage");
    auto *status=composer->findChild<QLabel *>("messageStatus");
    setComposerText(composer, "Continue after start");QTRY_VERIFY(send->isEnabled());
    QCOMPARE(send->text(),QString("Send"));QVERIFY(!send->icon().isNull());QVERIFY(status->text().contains("resume it or start fresh"));
    const auto choose=[&](const char *button){QTimer::singleShot(0,&window,[&,button]{auto *dialog=window.findChild<QMessageBox *>("startToSendConfirm");QVERIFY(dialog);
        QCOMPARE(dialog->defaultButton(),dialog->button(QMessageBox::Cancel));
        if(qstrcmp(button,"cancel")==0)dialog->button(QMessageBox::Cancel)->click();else dialog->findChild<QPushButton *>(button)->click();});};
    choose("cancel");send->click();QTest::qWait(100);QCOMPARE(resumes().size(),1);
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue after start"));
    // A resume that reports another conversation never receives the draft.
    choose("resumeAndSend");send->click();QTRY_COMPARE(writes.size(),2);
    QCOMPARE(resumes().last(),QString(R"(["resume", "codex/website/navigation", "-d"])"));
    QVERIFY(!send->isEnabled());QVERIFY(status->text().contains("Waiting for the session to start"));
    QCOMPARE(send->text(), QString("Waiting to send…")); QVERIFY(composer->editor()->isReadOnly());
    QVERIFY(window.findChild<QPushButton *>("cancelCompactSend")->isVisible());
    setState("running","run-resumed","conversation-other");window.setFleet(state);
    details=QJsonObject{{"tracked",true},{"run_id","run-resumed"},{"conversation_id","conversation-other"},{"runtime_state","live"},
        {"process_state","running"},{"activity","idle"},{"phase","idle"}};apply();
    QTRY_VERIFY(status->text().contains("unexpected conversation"));QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue after start"));
    // A fresh start delivers the unchanged draft once the new run accepts input.
    setState("stopped","run-old","conversation-old");window.setFleet(state);
    details=QJsonObject{{"tracked",true},{"run_id","run-old"},{"conversation_id","conversation-old"},{"runtime_state","saved"},{"process_state","exited"}};apply();
    QTRY_VERIFY(send->isEnabled());choose("startFreshAndSend");send->click();QTRY_COMPARE(writes.size(),3);QCOMPARE(resumes().last(),freshArgs);
    setState("running","run-new",{});window.setFleet(state);
    details=QJsonObject{{"tracked",true},{"run_id","run-new"},{"runtime_state","live"},{"process_state","running"},
        {"activity","idle"},{"phase","idle"},{"first_message_can_send",true}};apply();
    QTRY_VERIFY(QFileInfo::exists(temp.filePath("send.json")));
    QFile sent(temp.filePath("send.json"));QVERIFY(sent.open(QIODevice::ReadOnly));const auto payload=QJsonDocument::fromJson(sent.readAll()).object();
    QCOMPARE(payload["expected_run_id"].toString(),QString("run-new"));QVERIFY(QJsonDocument(payload).toJson().contains("Continue after start"));
    QVERIFY(!window.findChild<QMessageBox *>("coldCacheConfirm"));
    // A resumed conversation compacts before delivering the captured message.
    sent.close(); QTRY_VERIFY(!composer->isSending(composer->sessionKey()));
    QVERIFY(QFile::remove(temp.filePath("send.json")));
    setState("stopped","run-old","conversation-old");window.setFleet(state);
    details=QJsonObject{{"tracked",true},{"run_id","run-old"},{"conversation_id","conversation-old"},{"runtime_state","saved"},{"process_state","exited"}};apply();
    setComposerText(composer, "Continue with a compacted conversation");
    QTRY_VERIFY2(send->isEnabled(),qPrintable(status->text()));choose("resumeCompactAndSend");send->click();QTRY_COMPARE(writes.size(),4);
    QVERIFY(status->text().contains("then compact"));
    window.showSession({},"codex/hgs/dashboard");
    setComposerText(composer, "Keep the other dialog draft");
    QVERIFY(!window.findChild<QPushButton *>("cancelCompactSend")->isVisible());
    QVERIFY(!QFileInfo::exists(temp.filePath("compact-context.json")));
    setState("running","run-resumed","conversation-old");window.setFleet(state);
    details=QJsonObject{{"tracked",true},{"run_id","run-resumed"},{"conversation_id","conversation-old"},
        {"runtime_state","live"},{"process_state","running"},{"activity","idle"},{"phase","idle"},
        {"compact_context_supported",true},{"cache_hint",QJsonObject{{"status","cold"}}}};apply();
    QTRY_VERIFY(QFileInfo::exists(temp.filePath("compact-context.json")));
    QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));
    QFile compacted(temp.filePath("compact-context.json"));QVERIFY(compacted.open(QIODevice::ReadOnly));
    const auto compactPayload=QJsonDocument::fromJson(compacted.readAll()).object();
    QCOMPARE(compactPayload["expected_run_id"].toString(),QString("run-resumed"));
    QCOMPARE(compactPayload["expected_conversation_id"].toString(),QString("conversation-old"));
    details["compact_context_request"]=QJsonObject{{"status","completed"},{"request_id",compactPayload["request_id"]}};
    apply();QTRY_VERIFY(QFileInfo::exists(temp.filePath("send.json")));
    QFile finalSend(temp.filePath("send.json"));QVERIFY(finalSend.open(QIODevice::ReadOnly));
    const auto finalPayload=QJsonDocument::fromJson(finalSend.readAll()).object();
    QCOMPARE(finalPayload["text"].toString(),QString("Continue with a compacted conversation"));
    QCOMPARE(finalPayload["expected_compaction_id"],compactPayload["request_id"]);
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep the other dialog draft"));
    QVERIFY(!window.findChild<QMessageBox *>("coldCacheConfirm"));

}

void TestSessionsWindow::coldCacheCompactWaitsForSuccess_data()
{
    QTest::addColumn<QString>("outcome");
    for (const auto &value:{"completed","failed","cancelled","unchanged","draft","identity","cancel",
        "navigation","hidden","navigation-identity","navigation-cancel"}) QTest::newRow(value)<<QString(value);
}
void TestSessionsWindow::coldCacheCompactWaitsForSuccess()
{
    QFETCH(QString,outcome);
    QTemporaryDir temp;const auto program=temp.filePath("hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import sys,json,pathlib
root=pathlib.Path(__file__).parent
a=sys.argv[1:]
if a[0].startswith('@'):a=a[1:]
if a[0] in ['compact-context','send']:
 p=json.load(sys.stdin)
 if a[0]=='send':
  with (root/'send-targets.txt').open('a') as log:log.write(a[1]+'\n')
 staged=root/(a[0]+'.tmp');staged.write_text(json.dumps(p));staged.replace(root/(a[0]+'.json'))
 print(json.dumps(dict(request_id=p['request_id'],name=a[1],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],status='submitted')))
elif a[0]=='inspect':print((root/'details.json').read_text() if a[1]=='claude/infra/review' and (root/'details.json').exists() else '{}')
else:print('{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},{"activity","idle"},{"phase","idle"},
        {"runtime_state","live"},{"process_state","running"},{"compact_context_supported",true},{"cache_hint",QJsonObject{{"status","cold"},{"tokens",756000}}}};
    {QFile initial(temp.filePath("details.json"));QVERIFY(initial.open(QIODevice::WriteOnly));initial.write(QJsonDocument(details).toJson());}
    SessionsWindow window(FakeCli::program(program));window.setFleet(fleet());window.show();window.showSession("mac","claude/infra/review");QTest::qWait(100);
    // A completion that arrives while the held send is still being prepared is
    // picked up by the next compaction poll, which reads details.json: poll often.
    constexpr int compactPollMs = 200; window.setCompactPollIntervalForTests(compactPollMs);
    auto *client=window.findChild<HgsClient *>();
    // Polls read details.json while it changes: replace it atomically, or a torn read
    // fails an inspection and the held send is refused.
    const auto apply=[&]{QVERIFY(FakeCli::replace(temp.filePath("details.json"),QJsonDocument(details).toJson()));client->inspectionReady("mac","claude/infra/review",details);};apply();
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer, "Keep until compacted");
    QSignalSpy finished(client,&HgsClient::sessionActionFinished);
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<QMessageBox *>("coldCacheConfirm");QVERIFY(dialog);
        for(auto *button:dialog->buttons())if(button->text()=="Compact and continue"){QVERIFY(button->isEnabled());button->click();return;}QFAIL("No compact option");});
    QVERIFY(composer->findChild<QPushButton *>("sendMessage")->isEnabled());
    composer->findChild<QPushButton *>("sendMessage")->click();QTRY_COMPARE(finished.size(),1);QVERIFY(finished[0][1].toBool());
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep until compacted"));QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));
    QVERIFY(composer->editor()->isReadOnly()); QVERIFY(!composer->canAttachFiles());
    QCOMPARE(composer->findChild<QPushButton *>("sendMessage")->text(), QString("Waiting to send…"));
    QVERIFY(composer->findChild<QLabel *>("messageStatus")->text().contains("This exact message"));
    QTest::keyClicks(composer->editor(), "should not change the accepted message");
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep until compacted"));
    QFile request(temp.filePath("compact-context.json"));QVERIFY(request.open(QIODevice::ReadOnly));const auto payload=QJsonDocument::fromJson(request.readAll()).object();
    const auto id=payload.value("request_id").toString();
    details["compact_context_request"]=QJsonObject{{"request_id",id},{"status","compacting"}};details["activity"]="busy";details["phase"]="compacting";apply();
    QVERIFY(window.findChild<QPushButton *>("cancelCompactSend")->isVisible());QVERIFY(window.findChild<QPushButton *>("cancelCompactSend")->height()<=24);QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));
    if(!qEnvironmentVariable("HGS_COMPACT_PREVIEW").isEmpty())QVERIFY(window.grab().save(qEnvironmentVariable("HGS_COMPACT_PREVIEW")));
    if(outcome=="draft") { window.findChild<QPushButton *>("cancelCompactSend")->click(); setComposerText(composer, "Edited draft"); }
    if(outcome=="identity")details["conversation_id"]="another";
    const bool navigated = outcome.startsWith("navigation") || outcome == "hidden";
    const QString compactKey = composer->sessionKey();
    if (navigated) {
        // Only the background compaction poll can now continue the send.
        window.showSession({},"codex/hgs/dashboard");
        QVERIFY(!window.findChild<QPushButton *>("cancelCompactSend")->isVisible());
        setComposerText(composer, "Unrelated session draft");
        if (outcome == "navigation-cancel") {
            window.showSession("mac","claude/infra/review"); apply();
            QVERIFY(composer->editor()->isReadOnly());
            QCOMPARE(composer->editor()->toPlainText(),QString("Keep until compacted"));
            window.findChild<QPushButton *>("cancelCompactSend")->click();
            window.showSession({},"codex/hgs/dashboard");
        }
        if (outcome == "navigation-identity") details["conversation_id"]="another";
        if (outcome == "hidden") window.hide();
    }
    if(outcome=="cancel") {
        window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
        window.findChild<QPushButton *>("cancelCompactSend")->click();
        QVERIFY(!composer->editor()->isReadOnly()); QVERIFY(composer->canAttachFiles());
        QCOMPARE(QApplication::focusWidget(), composer->editor());
    }
    details["compact_context_request"]=QJsonObject{{"request_id",id},{"status",navigated||outcome=="draft"||outcome=="identity"||outcome=="cancel"?"completed":outcome}};
    details["activity"]="idle";details["phase"]="idle";
    if (navigated) QVERIFY(FakeCli::replace(temp.filePath("details.json"), QJsonDocument(details).toJson()));
    else apply();
    if(outcome=="completed" || outcome=="navigation" || outcome=="hidden") {
        QTRY_VERIFY(QFileInfo::exists(temp.filePath("send.json")));QFile sent(temp.filePath("send.json"));QVERIFY(sent.open(QIODevice::ReadOnly));
        const auto message=QJsonDocument::fromJson(sent.readAll()).object();QCOMPARE(message["text"].toString(),QString("Keep until compacted"));QCOMPARE(message["expected_compaction_id"].toString(),id);
        if (navigated) {
            QCOMPARE(composer->editor()->toPlainText(),QString("Unrelated session draft"));
            QVERIFY(!composer->editor()->isReadOnly());
            window.show(); window.showSession("mac","claude/infra/review"); apply();
            QTRY_VERIFY(composer->editor()->toPlainText().isEmpty());
            apply(); QTest::qWait(100);
        }
        QFile targets(temp.filePath("send-targets.txt")); QVERIFY(targets.open(QIODevice::ReadOnly));
        QCOMPARE(targets.readAll(),QByteArray("claude/infra/review\n"));
    } else if (navigated) {
        // A poll resolves the changed identity; a cancelled send stops polling.
        if (outcome == "navigation-identity") QTRY_VERIFY(!composer->isWaitingToSend(compactKey));
        QTest::qWait(3 * compactPollMs); QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));
        QCOMPARE(composer->editor()->toPlainText(),QString("Unrelated session draft"));
        window.showSession("mac","claude/infra/review"); apply();
        QCOMPARE(composer->editor()->toPlainText(),QString("Keep until compacted"));
        QVERIFY(!composer->editor()->isReadOnly());
    } else {QTest::qWait(100);QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));QCOMPARE(composer->editor()->toPlainText(),outcome=="draft"?QString("Edited draft"):QString("Keep until compacted"));
        // Input may stay locked for a moment after the outcome: a failed inspection, for
        // example, blocks it until the next poll (zerus-eyvo, seen once on a Windows runner).
        // Wait for the editor and name the lock if it stays; a send holding it leaves send.json.
        const auto lock=[&]{return QStringList{QString("waiting=%1").arg(composer->isWaitingToSend(composer->sessionKey())?"yes":"no"),
            QString("loading=%1").arg(composer->isDraftLoading(composer->sessionKey())?"yes":"no"),"send="+composer->findChild<QPushButton *>("sendMessage")->text(),
            "hint="+composer->editor()->toolTip(),"status="+composer->findChild<QLabel *>("messageStatus")->text()}.join("; ");};
        QTRY_VERIFY2(!composer->editor()->isReadOnly(),qPrintable(lock()));QVERIFY(!QFileInfo::exists(temp.filePath("send.json")));
        QCOMPARE(composer->editor()->toPlainText(),outcome=="draft"?QString("Edited draft"):QString("Keep until compacted"));QVERIFY(!composer->isWaitingToSend(composer->sessionKey()));}
}

void TestSessionsWindow::sharedRecoverySyncCatchesUpOfflinePeer()
{
    QTemporaryDir temp;const auto program=temp.filePath("sync-hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(QByteArray("#!/usr/bin/env python3\n")+R"PY(import sys,json,pathlib
root=pathlib.Path(__file__).parent
args=sys.argv[1:];host='local'
if args[0].startswith('@'):host=args.pop(0)[1:]
if host=='mac' and (root/'offline').exists():sys.exit(1)
path=root/(host+'.json')
p=json.loads(path.read_text())
def order(p):return [p['revision'],p['writer']]
if args[1]=='sync':
 incoming=json.load(sys.stdin)
 if order(incoming)>order(p):p=incoming;path.write_text(json.dumps(p))
print(json.dumps({'policy':p,'order':order(p)}))
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    const QJsonObject latest{{"version",3},{"revision",4},{"writer","local-writer"},{"enabled",false},{"service",true},{"network",true},{"rate_limit",true},
        {"session_limit",true},{"session_limit_mode","interval"},
        {"delays",QJsonArray{15,30,-1}},{"schedules",QJsonObject{{"network",QJsonArray{5,0}},{"session_limit",QJsonArray{600,0}}}}};
    auto older=latest;older["revision"]=2;older["writer"]="peer";
    for(const auto &host:QStringList{"local","mac"}){QFile file(temp.filePath(host+".json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write(QJsonDocument(host=="local"?latest:older).toJson());}
    QFile offline(temp.filePath("offline"));QVERIFY(offline.open(QIODevice::WriteOnly));offline.close();QSettings().remove("recovery/sharedPolicy");
    QObject parent;RecoverySync sync(FakeCli::program(program),&parent);RecoverySettings settings(&sync);sync.setPeers({"mac"});QTRY_VERIFY(!sync.busy());
    QCOMPARE(sync.envelope().value("policy").toObject(),latest);QVERIFY(sync.status().contains("mac"));
    QCOMPARE(settings.findChild<QComboBox *>("recoverySessionLimitMode")->currentData().toString(),QString("interval"));
    QCOMPARE(settings.findChild<QLineEdit *>("recoveryDelays_session_limit")->text(),QString("600, 0"));
    QVERIFY(QFile::remove(offline.fileName()));sync.refresh();QTRY_VERIFY(!sync.busy());
    QFile remote(temp.filePath("mac.json"));QVERIFY(remote.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(remote.readAll()).object(),latest);remote.close();
    auto newer=latest;newer["revision"]=5;newer["writer"]="remote-writer";newer["service"]=false;
    QVERIFY(remote.open(QIODevice::WriteOnly));remote.write(QJsonDocument(newer).toJson());remote.close();sync.refresh();QTRY_VERIFY(!sync.busy());
    QFile local(temp.filePath("local.json"));QVERIFY(local.open(QIODevice::ReadOnly));QCOMPARE(QJsonDocument::fromJson(local.readAll()).object(),newer);
    sync.save(latest,{{"policy",latest},{"order",QJsonArray{4,"local-writer"}}});QVERIFY(sync.error().contains("another Zerus"));
}

void TestSessionsWindow::recoverySettingsValidateAndSaveForSelectedMachine_data()
{
    QTest::addColumn<QString>("sessionMode");
    QTest::newRow("wait-for-reset") << QString("reset");
    QTest::newRow("interval") << QString("interval");
}

void TestSessionsWindow::recoverySettingsValidateAndSaveForSelectedMachine()
{
    QFETCH(QString, sessionMode);
    const auto program=m_dir->filePath("recovery-hgs"), capture=m_dir->filePath("recovery-policy.json");
    QFile::remove(capture);
    QFile fixture(program); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport sys\na=sys.argv[1:]\nif a[:1]==['@mac']:a=a[1:]\nif a[:1]!=['recovery']:sys.exit(2)\n"
                  "if a[1:2]==['set']:open("+FakeCli::pythonString(capture)+",'wb').write(sys.stdin.buffer.read())\n"
                  "print('{\"order\":[7,\"writer\"],\"policy\":{\"version\":2,\"revision\":7,\"writer\":\"writer\",\"enabled\":false,\"enabled_at\":0,\"service\":true,\"network\":true,\"rate_limit\":true,\"delays\":[15,30,60,300]}}')\n");
    fixture.close(); QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QSettings().remove("recovery/sharedPolicy");
    QSettings().setValue("workspace/theme","dark");SessionsWindow window(FakeCli::program(program));window.resize(1100,900);window.setFleet(fleet());window.show();
    window.findChild<QPushButton *>("workspaceSettings")->click();auto &page=*static_cast<SettingsPage *>(window.findChild<QWidget *>("settingsPage"));page.openRecovery();
    auto *enabled=page.findChild<QCheckBox *>("recoveryEnabled");QTRY_VERIFY(enabled->isEnabled());QVERIFY(!enabled->isChecked());
    auto *save=page.findChild<QPushButton *>("recoverySave");QTRY_VERIFY(save->isEnabled());
    auto *delays=page.findChild<QLineEdit *>("recoveryDelays_network");QCOMPARE(delays->text(),QString("15, 30, 60, 300, -1"));
    auto *limit=page.findChild<QCheckBox *>("recoveryRule_session_limit");QVERIFY(limit->isChecked());
    auto *mode=page.findChild<QComboBox *>("recoverySessionLimitMode");QCOMPARE(mode->currentData().toString(),QString("reset"));
    auto *limitDelays=page.findChild<QLineEdit *>("recoveryDelays_session_limit");QCOMPARE(limitDelays->text(),QString("300, 0"));QVERIFY(!limitDelays->isEnabled());
    const auto before=page.size();page.refresh();QTRY_VERIFY(save->isEnabled());QCOMPARE(page.size(),before);
    const auto preview=qEnvironmentVariable("HGS_SETTINGS_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/recovery.png"));window.resize(850,600);QTest::qWait(30);QVERIFY(window.grab().save(preview+"/recovery-narrow.png"));}
    enabled->setChecked(true);QVERIFY(mode->isEnabled());QVERIFY(!limitDelays->isEnabled());
    mode->setCurrentIndex(mode->findData(sessionMode));QCOMPARE(limitDelays->isEnabled(),sessionMode=="interval");
    if(sessionMode=="interval")limitDelays->setText("600, 0");
    limit->setChecked(false);QVERIFY(!mode->isEnabled());QVERIFY(!limitDelays->isEnabled());limit->setChecked(true);
    delays->setText("0");save->click();QVERIFY(!QFile::exists(capture));
    delays->setText("5, 20, 0");save->click();QTRY_VERIFY(QFile::exists(capture));
    const auto readPolicy=[&]{QFile saved(capture);if(!saved.open(QIODevice::ReadOnly))return QJsonObject();return QJsonDocument::fromJson(saved.readAll()).object();};
    QTRY_VERIFY(!readPolicy().isEmpty());const auto policy=readPolicy();
    QVERIFY(policy.value("enabled").toBool());QCOMPARE(policy.value("revision").toInt(),7);
    QCOMPARE(policy.value("version").toInt(),3);QVERIFY(policy.value("session_limit").toBool());QCOMPARE(policy.value("session_limit_mode").toString(),sessionMode);
    QCOMPARE(policy.value("schedules").toObject().value("session_limit").toArray(),QJsonArray({sessionMode=="interval"?600:300,0}));
    QCOMPARE(policy.value("schedules").toObject().value("network").toArray(),QJsonArray({5,20,0}));
    QCOMPARE(policy.value("schedules").toObject().value("service").toArray(),QJsonArray({15,30,60,300,-1}));
}

void TestSessionsWindow::sessionLimitRecoveryShowsResetAndKeepsDraft()
{
    SessionsWindow window(program());window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();auto *composer=window.findChild<MessageComposer *>("messageComposer");
    QTRY_VERIFY(composer->findChild<QLabel *>("messageStatus")->text().contains("starting"));
    setComposerText(composer, "Continue this draft after reset");
    window.activateWindow();composer->editor()->setFocus();QTRY_VERIFY(composer->editor()->hasFocus());
    const auto reset=QDateTime::currentMSecsSinceEpoch()/1000.0+7200;
    QJsonObject job{{"id","limit-job"},{"name","codex/hgs/dashboard"},{"class","session_limit"},{"session_limit_mode","reset"},
        {"state","waiting"},{"attempt",0},{"due_at",reset},{"not_before",reset},{"action","continue_message"}};
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"runtime_state","live"},{"process_state","running"},{"activity","attention"},{"phase","error"},
        {"last_event_at",2000000000},{"cursor",0},{"recovery",job},
        {"provider_error",QJsonObject{{"error_kind","session_limit"},{"detail","You've hit your session limit · resets 2:10pm (Europe/Moscow)"}}}};
    client->inspectionReady({},"codex/hgs/dashboard",details,{});
    auto *chip=window.findChild<ToolbarChip *>("recoveryChip");QVERIFY(chip->isVisible());
    QVERIFY(chip->fullLabel().contains("Waiting for reset"));QCOMPARE(chip->tone(),ChipTone::Warning);
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue this draft after reset"));QVERIFY(composer->editor()->hasFocus());
    QTest::mouseClick(chip,Qt::LeftButton);auto *panel=static_cast<RecoveryUi::Panel *>(window.findChild<QFrame *>("recoveryPanel"));QTRY_VERIFY(panel->isVisible());
    QVERIFY(panel->detailText().contains("Europe/Moscow"));QVERIFY(!panel->findChild<QPushButton *>("recoveryNow")->isEnabled());
    // Interval mode shows the ordinary countdown and permits an explicit retry before reset.
    job["session_limit_mode"]="interval";job["not_before"]=0;job["due_at"]=QDateTime::currentMSecsSinceEpoch()/1000.0+300;
    job["delays"]=QJsonArray{300,0};details["recovery"]=job;client->inspectionReady({},"codex/hgs/dashboard",details,{});
    QVERIFY(chip->fullLabel().contains("Retry in"));QVERIFY(panel->findChild<QPushButton *>("recoveryNow")->isEnabled());
    job["session_limit_mode"]="reset";job["state"]="blocked";job["reason"]="The reset time is unknown";
    details["recovery"]=job;client->inspectionReady({},"codex/hgs/dashboard",details,{});
    QVERIFY(panel->detailText().contains("reset time is unknown"));QVERIFY(!panel->findChild<QPushButton *>("recoveryNow")->isVisible());
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue this draft after reset"));
}

void TestSessionsWindow::composerNavigationDuringPolling()
{
    SessionsWindow window(program()); window.resize(1400, 900); window.setFleet(fleet()); window.show();
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspections(client, &HgsClient::inspectionReady);
    window.showSession({}, "codex/hgs/dashboard"); QTRY_VERIFY(!inspections.isEmpty());
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *editor = composer->editor(); window.activateWindow(); editor->setFocus(); QTRY_VERIFY(editor->hasFocus());
    const QString draft = QString::fromUtf8("1. вышел новый релиз zrok / ziti -- посмотри не надо ли обновить на сервере 2. посмотри йф");
    editor->insertPlainText(draft);
    const auto end = editor->cursorRect();
    QTest::keyClick(editor, Qt::Key_Home);
    QCOMPARE(editor->textCursor().position(), 0);
    for (int i = 0; i < 5; ++i) {
        window.setFleet(fleet());
        QTest::qWait(100);
        QCOMPARE(editor->textCursor().position(), 0);
        QVERIFY(editor->hasFocus());
    }
    QTest::keyClicks(editor, "X"); QCOMPARE(editor->toPlainText(), "X" + draft);
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, end.center());
    const int position = editor->textCursor().position();
    QVERIFY(position > 1);
    window.setFleet(fleet()); QTest::qWait(200);
    QCOMPARE(editor->textCursor().position(), position);
    auto expected = editor->toPlainText(); expected.insert(position, "Y");
    QTest::keyClicks(editor, "Y"); QCOMPARE(editor->toPlainText(), expected);
}

void TestSessionsWindow::refreshErrorsStayInComposerAndFooterWithoutMovingViews()
{
    const QJsonObject details{{"tracked",true},{"run_id","fixture-run"},{"conversation_id","fixture-conversation"},
        {"runtime_state","live"},{"process_state","running"},{"phase","idle"},{"activity","idle"},
        {"events",QJsonArray{}},{"cursor",0}};
    const auto fixture = programInspecting("inline-status", details); QVERIFY(!fixture.isEmpty());
    // A poll that succeeds clears the injected failure before the views are checked.
    SessionsWindow window(fixture); window.setFleet(fleet()); keepDetailsPollsOut(window); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    settleSelectedInspection(composer,client,{},"codex/hgs/dashboard",details);
    setComposerText(composer, "Keep my exact draft");
    auto cursor = composer->editor()->textCursor(); cursor.setPosition(4); cursor.setPosition(9, QTextCursor::KeepAnchor);
    composer->editor()->setTextCursor(cursor); composer->editor()->setFocus(); QTest::qWait(20);
    settleSelectedInspection(composer,client,{},"codex/hgs/dashboard",details);
    const auto tabPosition = tabs->mapTo(&window, QPoint()); const auto tabSize = tabs->size();
    const auto editorRect = composer->editor()->geometry();
    client->inspectionFailed({}, "codex/hgs/dashboard", "timed out"); QTest::qWait(20);
    QVERIFY(window.findChild<QLabel *>("hint")->isHidden());
    auto *status = composer->findChild<QLabel *>("messageStatus");
    QVERIFY(status->text().contains("Could not refresh activity: timed out")); QVERIFY(status->isVisible());
    QCOMPARE(tabs->mapTo(&window, QPoint()), tabPosition); QCOMPARE(tabs->size(), tabSize);
    QCOMPARE(composer->editor()->geometry(), editorRect); QVERIFY(composer->editor()->isReadOnly());
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep my exact draft"));
    QCOMPARE(composer->editor()->textCursor().position(), 9); QCOMPARE(composer->editor()->textCursor().anchor(), 4);
    const auto preview = qEnvironmentVariable("ZERUS_INLINE_STATUS_PREVIEW");
    if (!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(window.grab().save(preview + "/activity-error.png")); }
    auto *terminal = window.findChild<TerminalView *>(); tabs->setCurrentWidget(terminal); QTest::qWait(20);
    auto *footer = window.findChild<QLabel *>("sessionFeedback");
    QVERIFY(footer->isVisible()); QVERIFY(footer->text().contains("timed out"));
    QCOMPARE(tabs->mapTo(&window, QPoint()), tabPosition); QCOMPARE(tabs->size(), tabSize);
    if (!preview.isEmpty()) QVERIFY(window.grab().save(preview + "/terminal-error.png"));
    client->inspectionReady({}, "codex/hgs/dashboard", details); QTest::qWait(20);
    QVERIFY(footer->isHidden()); QVERIFY(!composer->editor()->isReadOnly());
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep my exact draft"));
}

void TestSessionsWindow::remoteSessionLoadingIsDelayedAndFollowsSelection()
{
    const auto fixture = m_dir->filePath("slow-session-read");
    QVERIFY(FakeCli::install(fixture, R"PY(#!/usr/bin/env python3
import json,sys,time
a=sys.argv[1:]
if 'inspect' in a:
 if a and a[0]=='@mac':time.sleep(2.8)
 print(json.dumps({'tracked':True,'run_id':'fixture-run','conversation_id':'fixture-conversation',
  'runtime_state':'live','process_state':'running','phase':'idle','activity':'idle','events':[],'cursor':0}))
else:print('{}')
)PY"));
    SessionsWindow window(FakeCli::program(fixture)); window.setFleet(fleet()); window.show();
    window.showSession({}, "codex/hgs/dashboard");
    auto *loading = window.findChild<QWidget *>("sessionLoading"); auto *title = window.findChild<QLabel *>("detailTitle");
    QVERIFY(loading); QVERIFY(title);
    window.setPollingHosts({QString()}); QTest::qWait(2100); QVERIFY(loading->isHidden()); window.setPollingHosts({});
    window.showSession("mac", "claude/infra/review"); QTest::qWait(100);
    const auto titleText = title->text(); const auto titleRect = title->geometry();
    QVERIFY(loading->isHidden()); QTest::qWait(700); QVERIFY(loading->isHidden());
    QTRY_VERIFY_WITH_TIMEOUT(loading->isVisible(), 1800);
    QCOMPARE(title->text(), titleText); QCOMPARE(title->geometry(), titleRect);
    QVERIFY(loading->geometry().left() >= title->fontMetrics().horizontalAdvance(titleText));
    const auto preview = qEnvironmentVariable("ZERUS_INLINE_STATUS_PREVIEW");
    if (!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(window.grab().save(preview + "/remote-loading.png")); }
    QTRY_VERIFY_WITH_TIMEOUT(loading->isHidden(), 2000);
    // The same remote read is still real work, but cannot decorate another selection.
    auto *client = window.findChild<HgsClient *>(); client->requestInspection("mac", "claude/infra/review");
    window.showSession({}, "codex/hgs/dashboard"); QTest::qWait(2200); QVERIFY(loading->isHidden());
    client->inspectionLoadingChanged("mac", "claude/infra/review", {}, {}, false); QVERIFY(loading->isHidden());
}

void TestSessionsWindow::connectionStatusSurvivesNoticesAndRetries()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.setNoticeDurationForTests(250);
    auto *status = window.findChild<QLabel *>("workspaceConnectionStatus");
    auto *retry = window.findChild<QPushButton *>("connectionRetry");
    QVERIFY(status); QVERIFY(retry); QVERIFY(status->text().isEmpty()); QVERIFY(retry->isHidden());
    window.setConnectionError("timed out");
    QVERIFY(status->text().contains("arch: session update timed out"));
    QVERIFY(status->text().contains("automatic retry pending"));
    QSignalSpy refresh(&window, &SessionsWindow::refreshRequested); retry->click(); QCOMPARE(refresh.size(), 1);
    window.setPollingHosts({QString()});
    QVERIFY(status->text().contains("retrying…")); QVERIFY(status->text().contains("timed out")); QVERIFY(!retry->isEnabled());
    window.showNotificationNotice("Path copied");
    QTRY_VERIFY(window.findChild<QLabel *>("notice")->text().isEmpty());
    QVERIFY(status->text().contains("timed out"));
    auto state = fleet(); BoxState unavailable; unavailable.host = "mac"; unavailable.error = "ssh: Could not resolve hostname <mac>";
    state.setPeer(unavailable, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(status->text().contains("<mac>")); QVERIFY(status->toolTip().contains("&lt;mac&gt;"));
    window.setConnectionError({}); window.setPollingHosts({});
    QVERIFY(!status->text().contains("arch:")); QVERIFY(status->text().contains("Could not resolve")); QVERIFY(retry->isEnabled());
    window.setPollingHosts({"mac"}); QVERIFY(status->text().contains("retrying…")); QVERIFY(!retry->isEnabled());
    window.setFleet(fleet()); window.setPollingHosts({});
    QVERIFY(status->text().isEmpty()); QVERIFY(retry->isHidden());
    state = fleet(); state.registerPeer("new-machine"); window.setFleet(state); window.setPollingHosts({"new-machine"});
    QVERIFY(status->text().contains("new-machine: connecting…"));
    const auto preview = qEnvironmentVariable("HGS_CONNECTION_PREVIEW");
    if (!preview.isEmpty()) { window.setConnectionError("timed out"); QTest::qWait(50); QVERIFY(window.grab().save(preview)); }
}

void TestSessionsWindow::peerNamedDifferentlyListsSessionsUnderItsAlias()
{
    // Saved as "winpeer", the machine calls itself "zpeer" (its HGS_SELF), and
    // `hgs @winpeer ls --json` forwards its own listing. The window is fed as
    // TrayAgent feeds it: the client's polls in flight and each answer's box.
    QTemporaryDir fixture; QVERIFY(fixture.isValid());
    const auto peerCli = fixture.filePath("hgs");
    QVERIFY(FakeCli::install(peerCli, "#!/usr/bin/env python3\nimport sys\n"
        "if sys.argv[1:]!=['@winpeer','ls','--json','--no-archives']:print('unexpected call',file=sys.stderr);sys.exit(1)\n"
        "print('{\"host\":\"zpeer\",\"ok\":true,\"projects\":{},\"sessions\":[{\"name\":\"codex/win/build\",\"cmd\":\"codex\","
        "\"project\":\"win\",\"tag\":\"build\",\"state\":\"running\",\"activity\":\"idle\"}]}')\n"));
    HgsClient client(FakeCli::program(peerCli)); QList<BoxState> answers;
    connect(&client, &HgsClient::peerReady, this, [&answers](const BoxState &box) { answers << box; });
    auto state = fleet(); auto local = state.local(); local.peersKnown = true; local.peers = {"mac", "winpeer"};
    state.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *status = window.findChild<QLabel *>("workspaceConnectionStatus"); QVERIFY(status);
    auto *list = window.findChild<QListWidget *>("sessionList"); QVERIFY(list);
    client.requestPeer("winpeer"); window.setPollingHosts(client.pollingHosts());
    QVERIFY(status->text().contains("winpeer: connecting…"));
    QTRY_COMPARE(answers.size(), 1);
    state.setPeer(answers.first(), QDateTime::currentMSecsSinceEpoch());
    window.setFleet(state); window.setPollingHosts(client.pollingHosts());
    QVERIFY2(status->text().isEmpty(), qPrintable(status->text()));
    QVERIFY(!state.peer("zpeer")); QVERIFY(state.peer("winpeer") && state.peer("winpeer")->ok);
    QListWidgetItem *row = nullptr;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(SessionRoles::Key).toString().endsWith("\ncodex/win/build")) row = list->item(i);
    QVERIFY(row);
    QCOMPARE(row->data(SessionRoles::Key).toString(), QString("winpeer\ncodex/win/build"));
    QCOMPARE(row->data(SessionRoles::MachineName).toString(), QString("winpeer"));
    QCOMPARE(row->data(SessionRoles::GitHost).toString(), QString("winpeer"));
}

void TestSessionsWindow::filtersSelectionAndOffline()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList"); QVERIFY(list);
    QCOMPARE(list->count(), 5); list->setCurrentRow(2);
    const auto key = list->currentItem()->data(Qt::UserRole);
    window.setFleet(fleet()); QCOMPARE(list->currentItem()->data(Qt::UserRole), key);
    auto *search = window.findChild<QLineEdit *>("search"); search->setText("navigation"); auto *results = window.findChild<QListWidget *>("searchResults"); QCOMPARE(results->count(), 1);
    search->setText("nothing matches"); QCOMPARE(results->count(), 0); search->clear();
    for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "attention") button->click();
    QCOMPARE(list->count(), 1); QVERIFY(list->item(0)->data(Qt::UserRole).toString().startsWith("mac\n"));
    auto state = fleet(); BoxState offline; offline.host = "mac"; offline.ok = false;
    state.setPeer(offline, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); QCOMPARE(list->count(), 0);
    for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "all") button->click();
    QCOMPARE(list->count(), 5); list->setCurrentRow(4);
    QVERIFY(!window.findChild<QAction *>("pauseAction")->isEnabled());
    QSignalSpy opened(&window, &SessionsWindow::sessionActivated);
    QTest::keyClick(list, Qt::Key_Return); QCOMPARE(opened.size(), 0);
    window.close(); // Closing a dashboard never invokes a session command.
}

void TestSessionsWindow::listFiltersKeepTheOpenConversation_data()
{
    QTest::addColumn<QString>("scope");
    for (const auto *scope : {"attention", "active", "paused", "archived", "machine", "project", "empty"})
        QTest::newRow(scope) << QString::fromLatin1(scope);
}

void TestSessionsWindow::listFiltersKeepTheOpenConversation()
{
    QFETCH(QString, scope);
    auto state = fleet(); auto local = state.local();
    auto archive = local.sessions[0]; archive.state = "archived"; archive.archiveId = "archive-one";
    archive.archivedAt = QDateTime::currentSecsSinceEpoch(); local.sessions.append(archive);
    state.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    const auto current = local.sessions[1]; const QString key = '\n' + current.name;
    SessionOrganization org; const auto otherProject = org.createGroup("Other project");
    org.moveSession(org.observe("mac", "claude/infra/review", ""), otherProject);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); window.setFleet(state); keepDetailsPollsOut(window);
    window.resize(1100,760); window.show(); window.showSession({}, current.name);
    auto *list = window.findChild<SessionList *>("sessionList");
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *title = window.findChild<QLabel *>("detailTitle"); const auto openedTitle = title->text();
    const QString draft = scope == "active" ? QString() : QStringLiteral("Keep the open session and this draft");
    setComposerText(composer, draft);
    auto cursor = composer->editor()->textCursor(); cursor.setPosition(qMin(5, int(draft.size())));
    cursor.setPosition(qMin(12, int(draft.size())), QTextCursor::KeepAnchor); composer->editor()->setTextCursor(cursor);
    const auto selection = cursor.selectedText();
    auto *machines = window.findChild<MachineFilter *>("machineFilterChips"); auto *projects = window.findChild<ProjectFilter *>("projectFilterChips");
    QHash<QString, QPushButton *> filters;
    for (auto *button : window.findChildren<QPushButton *>("sessionFilter")) filters[button->property("filter").toString()] = button;
    const auto applyScope = [&] {
        if (scope == "machine") machines->setSelection({"mac"});
        else if (scope == "project") projects->setSelection({otherProject});
        else if (scope == "empty") projects->setSelection({"unavailable-project"});
        else filters[scope]->click();
    };
    const auto clearScope = [&] { filters["all"]->click(); machines->setSelection({}); projects->setSelection({}); };
    applyScope();
    for (int i = 0; i < list->count(); ++i) QVERIFY(list->item(i)->data(SessionRoles::Key).toString() != key);
    QVERIFY(!list->currentItem());
    QCOMPARE(composer->sessionKey(), key); QCOMPARE(title->text(), openedTitle);
    QCOMPARE(composer->editor()->toPlainText(), draft); QCOMPARE(composer->editor()->textCursor().selectedText(), selection);
    // Regular fleet refreshes must not replace a conversation outside the list scope.
    window.setFleet(state);
    QCOMPARE(composer->sessionKey(), key); QCOMPARE(title->text(), openedTitle);
    QCOMPARE(composer->editor()->toPlainText(), draft); QCOMPARE(composer->editor()->textCursor().selectedText(), selection);
    clearScope();
    QVERIFY(list->currentItem()); QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(), key);
    QVERIFY(list->selectedItems().contains(list->currentItem()));
    QCOMPARE(composer->sessionKey(), key); QCOMPARE(composer->editor()->toPlainText(), draft);
    // An explicit row click still opens that target; the original draft remains recoverable.
    if (scope != "empty") applyScope();
    QListWidgetItem *next = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        const auto candidate = list->item(i)->data(SessionRoles::Key).toString();
        if (!list->item(i)->isHidden() && !list->item(i)->data(SessionRoles::Header).toBool()
            && !candidate.isEmpty() && candidate != key) { next = list->item(i); break; }
    }
    QVERIFY(next); const auto nextKey = next->data(SessionRoles::Key).toString(); list->scrollToItem(next);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(next).center());
    QCOMPARE(composer->sessionKey(), nextKey);
    window.showSession({}, current.name);
    QTRY_COMPARE(composer->editor()->toPlainText(), draft); QCOMPARE(composer->sessionKey(), key);
}

void TestSessionsWindow::activeFilterCombinesWorkAndAttentionOnce()
{
    auto state = fleet(); auto box = state.local();
    auto &working = box.sessions[0];
    auto &idle = box.sessions[1]; idle.conversationId = "idle-conversation"; idle.replyId = "idle-reply";
    auto overlap = working; overlap.name = "codex/hgs/overlap";
    overlap.conversationId = "overlap-conversation"; overlap.replyId = "overlap-reply";
    auto child = idle; child.name = "codex/hgs/child-request"; child.replyId.clear();
    child.subagentSource = "hooks"; child.subagents = {{"child", QJsonObject{{"display_state", "input"}}}};
    auto preview = idle; preview.name = "codex/hgs/preview-request"; preview.replyId.clear();
    preview.subagentSource = "hooks"; preview.subagentPreviews = {QJsonObject{{"state", "input"}}};
    auto reminder = box.sessions[2]; reminder.name = "codex/hgs/reminder"; reminder.reviewLater = true;
    auto exited = working; exited.name = "codex/hgs/exited"; exited.processState = "exited";
    auto ended = working; ended.name = "codex/hgs/ended"; ended.conversationState = "ended";
    auto unknown = working; unknown.name = "codex/hgs/unknown"; unknown.activity = "unknown";
    auto archived = overlap; archived.name = "codex/hgs/archive"; archived.state = "archived"; archived.archiveId = "archive-one";
    box.sessions.append({overlap, child, preview, reminder, exited, ended, unknown, archived});
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QVERIFY(state.setReviewLater({}, reminder, true));
    const auto previousMarks = QSettings().value("attention/sessionMarks");
    const auto restoreMarks = qScopeGuard([previousMarks] { QSettings().setValue("attention/sessionMarks", previousMarks); });
    QSettings().setValue("attention/sessionMarks", QJsonDocument(state.attentionMarks()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); window.setFleet(state); window.show();
    QPushButton *active = nullptr, *attention = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->property("filter") == "active") active = button;
        if (button->property("filter") == "attention") attention = button;
        QVERIFY(button->property("filter") != "working");
    }
    QVERIFY(active); QVERIFY(attention); QCOMPARE(active->accessibleName(), QString("Active"));
    // The working session's intermediate reply remains Active without adding attention.
    QCOMPARE(active->property("count").toInt(), 7); QCOMPARE(attention->property("count").toInt(), 5);
    auto *list = window.findChild<QListWidget *>("sessionList"); active->click(); QCOMPARE(list->count(), 7);
    QSet<QString> keys;
    for (int i = 0; i < list->count(); ++i) keys.insert(list->item(i)->data(Qt::UserRole).toString());
    QCOMPARE(keys.size(), 7); QVERIFY(keys.contains("\ncodex/hgs/dashboard"));
    QVERIFY(keys.contains("\nkimi/docs/research")); QVERIFY(keys.contains("mac\nclaude/infra/review"));
    QVERIFY(keys.contains("\ncodex/hgs/child-request")); QVERIFY(keys.contains("\ncodex/hgs/reminder"));
    QVERIFY(keys.contains("\ncodex/hgs/preview-request"));
    box.ok = false; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    auto peer = *state.peer("mac"); peer.ok = false; state.setPeer(peer, QDateTime::currentMSecsSinceEpoch());
    window.setFleet(state); QCOMPARE(active->property("count").toInt(), 1); QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QString("\ncodex/hgs/reminder"));
}

void TestSessionsWindow::backgroundShellRowKeepsActiveAndOpensItsProcesses()
{
    // A main Stop can leave two build/check commands running, as in the owner
    // report. Both the list and opened header retain Background until they exit.
    QSettings().setValue("processes/enabled", true);
    auto state = fleet(); auto box = state.local(); auto &s = box.sessions[1];
    s.activity = "idle"; s.phase = "idle"; s.backgroundProcesses = 2;
    s.backgroundStarted = QDateTime::currentSecsSinceEpoch() - 90;
    s.backgroundCommand = "build --wait"; s.scheduledJobs = 1;
    s.conversationId = "background-conversation"; s.replyId = "progress-reply"; s.sharedRead = true;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(script()); window.setFleet(state); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList");
    QListWidgetItem *row = nullptr;
    for (int i=0;i<list->count();++i) if (list->item(i)->data(SessionRoles::Key).toString()==QString("\n")+s.name) row=list->item(i);
    QVERIFY(row);
    QCOMPARE(row->data(SessionRoles::Status).toString(),QString("Background"));
    QCOMPARE(SessionDelegate::statusOf(list->model()->index(list->row(row),0),true).kind,SessionStatusBadge::Background);
    QVERIFY(row->data(SessionRoles::Working).toBool());
    QVERIFY(!row->data(SessionRoles::Unread).toBool());
    QVERIFY(state.local().sessions[1].unreadReply);
    QVERIFY(!row->data(SessionRoles::Scheduled).toBool());
    QCOMPARE(row->data(SessionRoles::WorkingSince).toDouble(),s.backgroundStarted);
    QVERIFY(row->data(SessionRoles::Detail).toString().contains("build --wait"));
    list->setCurrentItem(row);
    QSignalSpy terminated(&window,&SessionsWindow::sessionActivated);
    QContextMenuEvent event(QContextMenuEvent::Mouse,list->visualItemRect(row).center(),list->viewport()->mapToGlobal(list->visualItemRect(row).center()));
    QApplication::sendEvent(list->viewport(),&event);
    auto *menu=window.findChild<QMenu *>("sessionContextMenu"); QVERIFY(menu);
    auto *action=menu->findChild<QAction *>("contextOpenProcesses"); QVERIFY(action); QVERIFY(action->isEnabled());
    action->trigger(); menu->hide();
    QCOMPARE(window.findChild<QTabWidget *>("sessionDetailTabs")->currentWidget(),window.findChild<ProcessView *>());
    QCOMPARE(terminated.size(),0);
    auto *badge=window.findChild<QLabel *>("badge"); QVERIFY(badge);
    QVERIFY(badge->text().startsWith("Background"));
    QVERIFY(badge->width() >= badge->fontMetrics().horizontalAdvance(badge->text()) + 16);
    const QString preview=qEnvironmentVariable("ZERUS_BACKGROUND_PREVIEW");
    if(!preview.isEmpty())QVERIFY(window.grab().save(preview));
    box.sessions[1].backgroundProcesses=0; box.sessions[1].scheduledJobs=0;
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    for (int i=0;i<list->count();++i) if (list->item(i)->data(SessionRoles::Key).toString()==QString("\n")+s.name) {
        QCOMPARE(list->item(i)->data(SessionRoles::Status).toString(),QString("Ready"));
        QVERIFY(!list->item(i)->data(SessionRoles::Working).toBool());
        QVERIFY(list->item(i)->data(SessionRoles::Unread).toBool());
    }
}

void TestSessionsWindow::activeFilterDraftsRestoreOfflineAndFollowEditsAndDelivery()
{
    auto state = fleet(); auto box = state.local();
    for (auto &session : box.sessions) { session.phase = "idle"; session.activity = "idle"; }
    auto parent = box.sessions[0]; parent.name = "codex/hgs/child-parent"; parent.conversationId = "conversation-one";
    parent.subagentSource = "hooks"; parent.subagents = {{"child", QJsonObject{{"state", "idle"}}}};
    auto staleParent = parent; staleParent.name = "codex/hgs/stale-parent"; staleParent.conversationId = "new-conversation";
    auto archived = box.sessions[0]; archived.name = "codex/hgs/archived-draft"; archived.state = "archived"; archived.archiveId = "archive-one";
    box.sessions.append({parent, staleParent, archived}); state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    auto peer = *state.peer("mac"); peer.sessions[0].phase = "idle"; peer.sessions[0].activity = "idle";
    state.setPeer(peer, QDateTime::currentMSecsSinceEpoch()); peer.ok = false; state.setPeer(peer, QDateTime::currentMSecsSinceEpoch());
    ComposerDraftStore store; ComposerDraft text; text.text = "Restored draft";
    QVERIFY(store.save("\n" + box.sessions[2].name, text));
    QVERIFY(store.save("arch\n" + parent.name + "\nconversation-one\nchild", text));
    QVERIFY(store.save("arch\n" + staleParent.name + "\nconversation-one\nchild", text));
    QVERIFY(store.save("\n" + archived.name + "\narchive-one", text));
    ComposerDraft attachment; attachment.attachments = {{"notes.txt", "text/plain", "snapshot", {}}};
    QVERIFY(store.save("mac\n" + peer.sessions[0].name, attachment));
    SessionsWindow window(program()); window.setFleet(state); window.show();
    QPushButton *active = nullptr; QSet<QString> filterIds;
    for (auto *button : window.findChildren<QPushButton *>()) {
        const auto id = button->property("filter").toString();
        if (!id.isEmpty()) filterIds.insert(id);
        if (id == "active") active = button;
    }
    QCOMPARE(filterIds, QSet<QString>({"all", "attention", "active", "paused", "archived"}));
    QVERIFY(active); QTRY_COMPARE(active->property("count").toInt(), 3);
    auto *list = window.findChild<SessionList *>("sessionList"); active->click();
    QSet<QString> keys;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(SessionRoles::ChildId).toString().isEmpty()) keys.insert(list->item(i)->data(SessionRoles::Key).toString());
    QCOMPARE(keys, QSet<QString>({"\n" + box.sessions[2].name, "\n" + parent.name, "mac\n" + peer.sessions[0].name}));
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({"@local"}); QCOMPARE(active->property("count").toInt(), 2);
    window.findChild<MachineFilter *>("machineFilterChips")->setSelection({}); QCOMPARE(active->property("count").toInt(), 3);
    window.showSession({}, staleParent.name); QTest::qWait(50);
    QCOMPARE(active->property("count").toInt(), 3); // An older inspection cannot bind a child draft to a new parent conversation.
    window.showSession({}, box.sessions[1].name);
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    composer->editor()->setFocus(); setComposerText(composer, "New draft");
    QTRY_COMPARE(active->property("count").toInt(), 4);
    const auto key = composer->sessionKey(); QCOMPARE(key, "\n" + box.sessions[1].name);
    QVERIFY(composer->editor()->hasFocus());
    composer->editor()->clear(); QTRY_COMPARE(active->property("count").toInt(), 3);
    setComposerText(composer, "Send this draft"); QTRY_COMPARE(active->property("count").toInt(), 4);
    QVERIFY(composer->setSending(key)); QTRY_COMPARE(active->property("count").toInt(), 3);
    composer->deliveryFinished(key, false, "Synthetic delivery failure"); QTRY_COMPARE(active->property("count").toInt(), 4);
    QVERIFY(composer->setSending(key)); composer->deliveryFinished(key, true);
    QTRY_COMPARE(active->property("count").toInt(), 3);
}

void TestSessionsWindow::messageDeliveryKeepsSessionIdentityAndDrafts_data()
{
    QTest::addColumn<QString>("promptEvent");
    QTest::addColumn<bool>("busy");
    QTest::addColumn<bool>("providerFailure");
    QTest::newRow("submitted") << QString("UserPromptSubmit") << false << false;
    QTest::newRow("working") << QString("UserPromptSubmit") << true << false;
    QTest::newRow("queued") << QString("UserPromptQueued") << true << false;
    QTest::newRow("kimi-turn-started") << QString("TurnStarted") << false << false;
    QTest::newRow("provider-error-retry") << QString("UserPromptSubmit") << false << true;
}

void TestSessionsWindow::modelSettingsKeepSessionIdentity_data()
{
    QTest::addColumn<QString>("state"); QTest::addColumn<QString>("applyError");
    QTest::addColumn<bool>("preflightRefusal"); QTest::addColumn<bool>("keepSelected");
    QTest::newRow("paused") << QString("paused") << QString() << false << false;
    QTest::newRow("stopped") << QString("stopped") << QString() << false << false;
    QTest::newRow("busy-background-applied") << QString("running") << QString() << false << false;
    QTest::newRow("busy-background-failed-once") << QString("running") << QString("Native model picker unavailable") << false << false;
    QTest::newRow("busy-background-consumed") << QString("running")
        << QString("hgs: pending settings changed; refresh before applying") << true << false;
    QTest::newRow("busy-selected-consumed") << QString("running")
        << QString("pending settings changed; refresh before applying") << true << true;
    QTest::newRow("busy-background-target-changed") << QString("running")
        << QString("pending settings target changed; refresh before applying") << true << false;
    QTest::newRow("busy-selected-target-changed") << QString("running")
        << QString("hgs: pending settings target changed; refresh before applying") << true << true;
    QTest::newRow("busy-background-native-error-with-similar-detail") << QString("running")
        << QString("settings change not confirmed: pending settings changed; refresh before applying") << false << false;
}

void TestSessionsWindow::modelSettingsKeepSessionIdentity()
{
    QFETCH(QString, state); QFETCH(QString, applyError);
    QFETCH(bool, preflightRefusal); QFETCH(bool, keepSelected);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString program = directory.filePath("hgs"), snapshot = directory.filePath("snapshot.json"), capture = directory.filePath("calls.jsonl");
    QJsonObject details{{"tracked", true}, {"run_id", "run-original"}, {"conversation_id", "conversation-one"},
        {"activity", "busy"}, {"phase", "tool"}, {"model", "gpt-6.1-sol"}, {"effort", "high"},
        {"settings_change_supported", true}, {"settings_apply_when", state == "running" ? "ready" : "resume"},
        {"model_options", QJsonArray{QJsonObject{{"id", "gpt-6.1-sol"}, {"effort_options", QJsonArray{"high", "xhigh"}}}}},
        {"cursor", 0}, {"events", QJsonArray{}}};
    const auto saveSnapshot = [&] { return FakeCli::replace(snapshot, QJsonDocument(details).toJson()); };
    QVERIFY(saveSnapshot());
    QFile executable(program); QVERIFY(executable.open(QIODevice::WriteOnly));
    QByteArray body = "#!/usr/bin/env python3\nimport json,sys,pathlib,time\nroot=pathlib.Path(__file__).parent\nargs=sys.argv[1:]\n";
    body += "apply_error=" + QJsonDocument(QJsonArray{applyError}).toJson(QJsonDocument::Compact) + "[0]\n";
    body += preflightRefusal ? "preflight=True\n" : "preflight=False\n";
    // Polls read snapshot.json while a settings call rewrites it: replace it, so no
    // inspection reads a truncated file and fails. Windows refuses the rename while
    // a poll has the file open; retry then.
    body += R"PY(def publish(data):
 staged=root/'snapshot.tmp';staged.write_text(json.dumps(data))
 for _ in range(250):
  try:return staged.replace(root/'snapshot.json')
  except PermissionError:time.sleep(.02)
 sys.exit('snapshot.json stayed open')
if args[0]=='inspect':
 print((root/'snapshot.json').read_text())
elif args[0]=='settings':
 p=json.load(sys.stdin)
 calls=root/'calls.jsonl'
 previous=calls.read_text().count('\n') if calls.exists() else 0
 with calls.open('a') as out: out.write(json.dumps({'args':args,'payload':p})+'\n')
 time.sleep(.12)
 data=json.loads((root/'snapshot.json').read_text())
 if previous and apply_error:
  if preflight:
   # A send or another client consumed the pending request before this guard.
   data['model']=p['model'];data['effort']=p['effort']
   for field in ['pending_model','pending_effort','pending_settings_id']:data.pop(field,None)
   publish(data)
  print(apply_error,file=sys.stderr);sys.exit(1)
 result='applied' if previous else 'scheduled'
 if result=='scheduled':
  data['pending_model']=p['model'];data['pending_effort']=p['effort'];data['pending_settings_id']=p['request_id']
 else:
  data['model']=p['model'];data['effort']=p['effort'];data.pop('pending_model',None);data.pop('pending_effort',None)
 publish(data)
 print(json.dumps({'status':result,'request_id':p['request_id'],'name':args[1],'run_id':p['expected_run_id'],
 'conversation_id':p['expected_conversation_id'],'model':p['model'],'effort':p['effort']}))
)PY";
    executable.write(body); executable.close(); QVERIFY(FakeCli::makeRunnable(executable.fileName()));
    auto current = fleet(); auto box = current.local(); box.sessions[0].state = state; box.sessions[0].runId = "run-original";
    current.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(program)); window.setFleet(current); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspections(client, &HgsClient::inspectionReady), changes(client, &HgsClient::settingsFinished);
    QTRY_VERIFY(!inspections.isEmpty());
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Saved draft for original session");
    QVERIFY(composer->addAttachment("notes.txt", "text/plain", "keep"));
    auto *settings = window.findChild<MessageComposer *>("messageComposer")->findChild<QPushButton *>("sessionModelSettings"); settings->click();
    auto *efforts = window.findChild<MessageComposer *>("messageComposer")->findChild<QComboBox *>("sessionEffortChoice"); efforts->setCurrentIndex(efforts->findData("xhigh"));
    auto *apply = window.findChild<MessageComposer *>("messageComposer")->findChild<QPushButton *>("applySessionSettings"); QVERIFY(apply->isEnabled()); apply->click();
    if (!keepSelected) { window.showSession({}, "kimi/docs/research"); setComposerText(composer, "Other session draft"); }
    const QString currentDraft = keepSelected ? "[File #1] Saved draft for original session" : "Other session draft";
    QTRY_COMPARE(changes.size(), 1); QVERIFY(changes[0][1].toBool()); QCOMPARE(composer->editor()->toPlainText(), currentDraft);
    if (state == "running") {
        box.sessions[0].phase = "idle"; box.sessions[0].activity = "idle"; box.sessions[0].processState = "running";
        current.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(current);
        QTRY_COMPARE(changes.size(), 2); QCOMPARE(changes[1][1].toBool(), applyError.isEmpty());
        auto *notice = window.findChild<QLabel *>("notice"); QVERIFY(notice);
        if (!applyError.isEmpty() && !preflightRefusal) {
            QVERIFY(notice->isVisible()); QVERIFY(notice->property("error").toBool());
            QVERIFY(notice->text().contains(applyError));
        } else QVERIFY(notice->isHidden());
        window.setFleet(current); window.setFleet(current); QTest::qWait(80); QCOMPARE(changes.size(), 2);
        QCOMPARE(composer->editor()->toPlainText(), currentDraft);
    } else {
        window.setFleet(current); QTest::qWait(80); QCOMPARE(changes.size(), 1);
    }
    QFile recorded(capture); QVERIFY(recorded.open(QIODevice::ReadOnly));
    QString originalRequest;
    for (const auto &line : recorded.readAll().split('\n')) {
        if (line.isEmpty()) continue;
        const auto call = QJsonDocument::fromJson(line).object(), payload = call.value("payload").toObject();
        QCOMPARE(call.value("args").toArray(), QJsonArray({"settings", "codex/hgs/dashboard", "--json"}));
        QCOMPARE(payload.value("expected_run_id").toString(), QString("run-original"));
        QCOMPARE(payload.value("expected_conversation_id").toString(), QString("conversation-one"));
        QCOMPARE(payload.value("effort").toString(), QString("xhigh"));
        if (originalRequest.isEmpty()) { QVERIFY(!payload.contains("expected_pending_id")); originalRequest = payload.value("request_id").toString(); }
        else QCOMPARE(payload.value("expected_pending_id").toString(), originalRequest);
    }
    window.showSession({}, "codex/hgs/dashboard");
    if (preflightRefusal) QTRY_VERIFY(!settings->toolTip().contains("Pending:"));
    QCOMPARE(composer->editor()->toPlainText(), QString("[File #1] Saved draft for original session"));
    QCOMPARE(composer->findChildren<QPushButton *>("removeAttachment").size(), 1);
    QTest::qWait(80); QCOMPARE(changes.size(), state == "running" ? 2 : 1);
}

void TestSessionsWindow::scheduledSettingsApplyOncePerReady_data()
{
    // What the attempt at the next Ready returns. There is none when the run or
    // its conversation changes first.
    // Without listedChooser only inspect of the selected session shows the
    // chooser, as when it closed again between two fleet listings.
    QTest::addColumn<bool>("keepSelected"); QTest::addColumn<QString>("retry"); QTest::addColumn<QString>("changed");
    QTest::addColumn<bool>("listedChooser");
    QTest::newRow("background-applied") << false << QString("applied") << QString() << true;
    QTest::newRow("background-failed") << false << QString("Native model picker unavailable") << QString() << true;
    QTest::newRow("background-new-run") << false << QString() << QString("run") << true;
    QTest::newRow("background-new-conversation") << false << QString() << QString("conversation") << true;
    QTest::newRow("selected-applied") << true << QString("applied") << QString() << true;
    QTest::newRow("selected-inspected-chooser") << true << QString("applied") << QString() << false;
}

void TestSessionsWindow::scheduledSettingsApplyOncePerReady()
{
    // Each settings change and inspection runs the fake CLI, which is slow on Windows runners.
    constexpr int SettingsTimeout = 20000;
    // A chooser can appear between the idle listing and the dispatch of a
    // pending change; dispatch then schedules it again. The change stays
    // pending for one attempt per Ready transition, never a polling loop.
    QFETCH(bool, keepSelected); QFETCH(QString, retry); QFETCH(QString, changed); QFETCH(bool, listedChooser);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const QString program = directory.filePath("hgs"), snapshot = directory.filePath("snapshot.json"), capture = directory.filePath("calls.jsonl");
    QJsonObject details{{"tracked", true}, {"run_id", "run-original"}, {"conversation_id", "conversation-one"},
        {"activity", "busy"}, {"phase", "tool"}, {"model", "gpt-6.1-sol"}, {"effort", "high"},
        {"settings_change_supported", true}, {"settings_apply_when", "ready"},
        {"model_options", QJsonArray{QJsonObject{{"id", "gpt-6.1-sol"}, {"effort_options", QJsonArray{"high", "xhigh"}}}}},
        {"cursor", 0}, {"events", QJsonArray{}}};
    QVERIFY(FakeCli::replace(snapshot, QJsonDocument(details).toJson()));
    QFile executable(program); QVERIFY(executable.open(QIODevice::WriteOnly));
    QByteArray body = "#!/usr/bin/env python3\nimport json,sys,pathlib,time\nroot=pathlib.Path(__file__).parent\nargs=sys.argv[1:]\n";
    body += "outcomes=['scheduled','scheduled']+" + QJsonDocument(QJsonArray{retry}).toJson(QJsonDocument::Compact) + "\n";
    // As in modelSettingsKeepSessionIdentity: replace snapshot.json atomically.
    body += R"PY(def publish(data):
 staged=root/'snapshot.tmp';staged.write_text(json.dumps(data))
 for _ in range(250):
  try:return staged.replace(root/'snapshot.json')
  except PermissionError:time.sleep(.02)
 sys.exit('snapshot.json stayed open')
if args[0]=='inspect':
 if args[1]=='codex/hgs/dashboard':print((root/'snapshot.json').read_text())
 else:print(json.dumps({'tracked':True,'run_id':'run-other','conversation_id':'conversation-other','activity':'idle','phase':'idle','cursor':0,'events':[]}))
elif args[0]=='settings':
 p=json.load(sys.stdin)
 calls=root/'calls.jsonl'
 previous=calls.read_text().count('\n') if calls.exists() else 0
 with calls.open('a') as out: out.write(json.dumps({'args':args,'payload':p})+'\n')
 time.sleep(.12)
 result=outcomes[previous] if previous<len(outcomes) else 'unexpected settings call'
 data=json.loads((root/'snapshot.json').read_text())
 if result=='scheduled':
  # The terminal shows a chooser: dispatch schedules, and an apply of the
  # pending change stays scheduled.
  if 'expected_pending_id' not in p:
   data['pending_model']=p['model'];data['pending_effort']=p['effort'];data['pending_settings_id']=p['request_id'];publish(data)
  pending=p.get('expected_pending_id',p['request_id'])
 elif result=='applied':
  data['model']=p['model'];data['effort']=p['effort']
  for field in ['pending_model','pending_effort','pending_settings_id']:data.pop(field,None)
  publish(data)
 else:
  print(result,file=sys.stderr);sys.exit(1)
 receipt={'status':result,'request_id':p['request_id'],'name':args[1],'run_id':p['expected_run_id'],
  'conversation_id':p['expected_conversation_id'],'model':p['model'],'effort':p['effort']}
 if result=='scheduled':receipt.update(pending_settings_id=pending,apply_when='ready')
 print(json.dumps(receipt))
)PY";
    executable.write(body); executable.close(); QVERIFY(FakeCli::makeRunnable(executable.fileName()));
    auto current = fleet(); auto box = current.local(); box.sessions[0].runId = "run-original";
    current.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(program)); window.setDetailsPollForTests(100, 200);
    window.setFleet(current); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspections(client, &HgsClient::inspectionReady), changes(client, &HgsClient::settingsFinished);
    QTRY_VERIFY(!inspections.isEmpty());
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    composer->findChild<QPushButton *>("sessionModelSettings")->click();
    auto *efforts = composer->findChild<QComboBox *>("sessionEffortChoice"); efforts->setCurrentIndex(efforts->findData("xhigh"));
    auto *apply = composer->findChild<QPushButton *>("applySessionSettings"); QVERIFY(apply->isEnabled()); apply->click();
    if (!keepSelected) window.showSession({}, "kimi/docs/research");
    QTRY_COMPARE_WITH_TIMEOUT(changes.size(), 1, SettingsTimeout); QVERIFY(changes[0][1].toBool());
    QCOMPARE(changes[0][2].toJsonObject().value("status").toString(), QString("scheduled"));
    const auto list = [&](const QString &activity, const QString &phase) {
        box.sessions[0].activity = activity; box.sessions[0].phase = phase; box.sessions[0].processState = "running";
        current.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(current);
    };
    const auto calls = [&] {
        QFile recorded(capture); return recorded.open(QIODevice::ReadOnly) ? recorded.readAll().count('\n') : -1;
    };
    const auto published = [&] {
        QFile file(snapshot); return QJsonDocument::fromJson(file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray()).object();
    };
    // Waits for two of the selected session's inspections, then counts calls.
    QString selectedName = keepSelected ? "codex/hgs/dashboard" : "kimi/docs/research";
    const auto settledCalls = [&] {
        const auto seen = inspections.size(); QTest::qWait(150);
        const auto inspected = [&] { return std::count_if(inspections.cbegin() + seen, inspections.cend(),
            [&](const QList<QVariant> &reply) { return reply[1].toString() == selectedName; }); };
        if (!QTest::qWaitFor([&] { return inspected() >= 2; }, SettingsTimeout)) return -1;
        return changes.size() == calls() ? int(changes.size()) : -1;
    };
    // The idle listing starts the automatic apply, but a chooser appeared in
    // the meantime: dispatch schedules it.
    list("idle", "idle");
    QTRY_COMPARE_WITH_TIMEOUT(changes.size(), 2, SettingsTimeout); QVERIFY(changes[1][1].toBool());
    QCOMPARE(changes[1][2].toJsonObject().value("status").toString(), QString("scheduled"));
    // No second attempt while the session stays Ready or shows the chooser.
    list("idle", "idle"); list("idle", "idle"); QCOMPARE(settledCalls(), 2);
    if (listedChooser) {
        list("busy", "input"); QCOMPARE(settledCalls(), 2);
        list("busy", "input"); QCOMPARE(settledCalls(), 2);
    }
    if (!changed.isEmpty()) {
        // A new run or conversation never receives the earlier one's change.
        if (changed == "run") box.sessions[0].runId = "run-two";
        else box.sessions[0].conversationId = "conversation-two";
        list("idle", "idle"); list("busy", "tool"); list("idle", "idle");
        QCOMPARE(settledCalls(), 2);
    } else {
        list("idle", "idle");
        if (keepSelected) {
            // The selected session also needs inspect to report Ready.
            QCOMPARE(settledCalls(), 2);
            auto ready = published();
            QVERIFY(ready.contains("pending_settings_id")); ready["settings_apply_when"] = "now";
            QVERIFY(FakeCli::replace(snapshot, QJsonDocument(ready).toJson()));
        }
        QTRY_COMPARE_WITH_TIMEOUT(changes.size(), 3, SettingsTimeout); QCOMPARE(changes[2][1].toBool(), retry == "applied");
        auto *notice = window.findChild<QLabel *>("notice"); QVERIFY(notice);
        if (retry == "applied") QVERIFY(notice->isHidden());
        else { QVERIFY(notice->isVisible()); QVERIFY(notice->text().contains(retry)); }
        // Applied or failed: later Ready transitions start nothing.
        list("busy", "tool"); list("idle", "idle"); list("busy", "input"); list("idle", "idle");
        QCOMPARE(settledCalls(), 3);
    }
    const auto attempts = changed.isEmpty() ? 3 : 2;
    // Selecting the session with inspect reporting Ready starts nothing either.
    auto ready = published();
    ready["settings_apply_when"] = "now"; QVERIFY(FakeCli::replace(snapshot, QJsonDocument(ready).toJson()));
    selectedName = "codex/hgs/dashboard"; window.showSession({}, selectedName); QCOMPARE(settledCalls(), attempts);
    QFile recorded(capture); QVERIFY(recorded.open(QIODevice::ReadOnly));
    QString pendingId;
    for (const auto &line : recorded.readAll().split('\n')) {
        if (line.isEmpty()) continue;
        const auto payload = QJsonDocument::fromJson(line).object().value("payload").toObject();
        QCOMPARE(payload.value("expected_run_id").toString(), QString("run-original"));
        QCOMPARE(payload.value("effort").toString(), QString("xhigh"));
        if (pendingId.isEmpty()) { QVERIFY(!payload.contains("expected_pending_id")); pendingId = payload.value("request_id").toString(); }
        else QCOMPARE(payload.value("expected_pending_id").toString(), pendingId);
    }
}

void TestSessionsWindow::messageDeliveryKeepsSessionIdentityAndDrafts()
{
    QFETCH(QString, promptEvent);
    QFETCH(bool, busy);
    QFETCH(bool, providerFailure);
    const QString capture = m_dir->filePath("message-capture.json");
    const QString program = m_dir->filePath("message-hgs");
    QFile::remove(program + ".details"); // Each data row starts with fresh native state.
    QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray captureLiteral = QJsonDocument(QJsonArray{capture}).toJson(QJsonDocument::Compact);
    QByteArray body = "#!/usr/bin/env python3\nimport sys,json,time,pathlib\nargs=sys.argv[1:]\nif args[0].startswith('@'): args=args[1:]\n";
    body += "capture=" + captureLiteral + "[0]\n";
    body += R"PY(if args[0]=='inspect':
 snapshot=pathlib.Path(__file__+'.details')
 print(snapshot.read_text() if snapshot.exists() else json.dumps({'tracked':True,'run_id':'run-ready','conversation_id':'conversation-ready','runtime_state':'live','process_state':'running','activity':'idle','phase':'idle','last_event_at':2000000000,'events':[],'cursor':0}))
elif args[0]=='send':
 payload=json.load(sys.stdin)
 with open(capture,'w') as f: json.dump({'args':args,'payload':payload},f)
 time.sleep(0.15)
 print(json.dumps({'status':'submitted','request_id':payload['request_id'],'name':args[1],'run_id':payload['expected_run_id'],'conversation_id':payload['expected_conversation_id'],'submitted_text':payload['text'],'submitted_at':time.time()}))
)PY";
    if (busy) body.replace("'activity':'idle','phase':'idle'", "'activity':'busy','phase':'tool'");
    if (providerFailure) body.replace("'activity':'idle','phase':'idle'",
        "'activity':'attention','phase':'error','provider_error':{'error_kind':'capacity'},'error_message_can_send':True");
    file.write(body); file.close(); QVERIFY(FakeCli::makeRunnable(file.fileName()));
    SessionsWindow window(FakeCli::program(program)); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    auto *send = window.findChild<MessageComposer *>("messageComposer")->findChild<QPushButton *>("sendMessage"); QVERIFY(send);
    auto *client = window.findChild<HgsClient *>(); QVERIFY(client);
    const auto applyDetails = [&](const QJsonObject &details) {
        QVERIFY(FakeCli::replace(program + ".details", QJsonDocument(details).toJson()));
        // Drain old replies, then start an inspection reading the new atomic
        // snapshot. All later automatic polls now agree with the test state.
        QSignalSpy replies(client, &HgsClient::inspectionReady); qsizetype issued = -1;
        QTRY_VERIFY(issued >= 0 || (client->requestInspection({}, "codex/hgs/dashboard") && (issued = replies.size(), true)));
        QTRY_VERIFY(std::any_of(replies.cbegin() + issued, replies.cend(), [](const QList<QVariant> &reply) {
            return reply[0].toString().isEmpty() && reply[1].toString() == "codex/hgs/dashboard" && reply[3].toString().isEmpty();
        }));
        client->inspectionReady({}, "codex/hgs/dashboard", details);
    };
    QSignalSpy delivered(client, &HgsClient::messageSent);
    const QString message = QString::fromUtf8("Review this screenshot.\nКавычки: '$HOME' `whoami`");
    setComposerText(composer, message);
    QVERIFY(composer->addAttachment("notes.txt", "text/plain", QByteArray("review notes")));
    QTRY_VERIFY(send->isEnabled()); window.activateWindow(); composer->editor()->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(),composer->editor());
    QTest::keyClick(composer->editor(),Qt::Key_Return); QVERIFY(!send->isEnabled());
    QCOMPARE(QApplication::focusWidget(),composer->editor());
    QCOMPARE(delivered.size(), 0); // The transport has not acknowledged this request yet.
    QTRY_VERIFY(composer->editor()->toPlainText().isEmpty());
    QCOMPARE(composer->findChildren<QPushButton *>("removeAttachment").size(), 0);
    QTRY_VERIFY(window.findChild<QTextBrowser *>("activity")->toPlainText().contains("Review this screenshot."));
    if(promptEvent=="UserPromptQueued") {
        QTRY_COMPARE(delivered.size(),1);
        QCOMPARE(QApplication::focusWidget(),composer->editor());QVERIFY(!composer->editor()->isReadOnly());
    }
    window.showSession({}, "kimi/docs/research"); setComposerText(composer, "Separate Kimi draft");
    QTRY_COMPARE(delivered.size(), 1);
    QCOMPARE(composer->editor()->toPlainText(), QString("Separate Kimi draft"));
    QFile captured(capture); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto result = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(result.value("args").toArray().at(1).toString(), QString("codex/hgs/dashboard"));
    const auto payload = result.value("payload").toObject();
    QCOMPARE(payload.value("text").toString(), "[File #1] " + message);
    QCOMPARE(payload.value("attachments").toArray()[0].toObject().value("reference").toString(), QString("[File #1]"));
    QCOMPARE(payload.value("expected_run_id").toString(), QString("run-ready"));
    QCOMPARE(payload.value("expected_conversation_id").toString(), QString("conversation-ready"));
    QCOMPARE(QByteArray::fromBase64(payload.value("attachments").toArray().at(0).toObject().value("data_base64").toString().toLatin1()), QByteArray("review notes"));
    window.showSession({}, "codex/hgs/dashboard");
    QCOMPARE(composer->editor()->toPlainText(), QString());
    QJsonObject details{{"tracked", true}, {"run_id", "run-ready"}, {"conversation_id", "conversation-ready"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"activity", "idle"}, {"phase", "idle"}, {"last_event_at", 2000000000},
        {"cursor", 1}, {"events", QJsonArray{QJsonObject{{"seq", 1}, {"at", QDateTime::currentMSecsSinceEpoch() / 1000.0},
        {"type", promptEvent}, {"detail", payload.value("text")}}}}};
    applyDetails(details);
    auto *activity = window.findChild<QTextBrowser *>("activity"); QVERIFY(activity);
    QTRY_VERIFY(!window.findChild<ActivityView *>("mainActivity")->renderPending());
    QTRY_COMPARE(activity->toPlainText().count("Review this screenshot."), 1);
    // The native attachment receipt replaces the local acknowledgement in one
    // poll. Only the file's storage metadata changes; the visible card should
    // not be replaced at all, and must never transiently appear twice.
    auto attachment = QJsonObject{{"name","notes.txt"},{"mime","text/plain"},{"bytes",12},
        {"reference","[File #1]"},{"request_id",payload["request_id"]},{"index",0}};
    details["attachment_messages"] = QJsonArray{QJsonObject{{"type",promptEvent},{"source","hgs_delivery"},
        {"message_id",payload["request_id"]},{"at",details["events"].toArray().first().toObject()["at"]},
        {"detail",payload["text"]},{"submitted_text",payload["text"]},{"attachments",QJsonArray{attachment}}}};
    details["events"] = QJsonArray{};
    QSignalSpy documentChanges(activity, &QTextBrowser::textChanged);
    applyDetails(details);
    QTRY_VERIFY(!window.findChild<ActivityView *>("mainActivity")->renderPending());
    QCOMPARE(documentChanges.size(), 0);
    QTRY_COMPARE(activity->toPlainText().count("Review this screenshot."), 1);
    window.showSession({}, "codex/website/navigation");
    setComposerText(composer, "Draft for a saved session");
    // Send stays available; it asks how to start the saved session first.
    QVERIFY(send->isEnabled());
    QVERIFY(composer->findChild<QLabel *>("messageStatus")->text().contains("resume it or start fresh"));
    window.showSession({}, "kimi/docs/research");
    QCOMPARE(composer->editor()->toPlainText(), QString("Separate Kimi draft"));
}

void TestSessionsWindow::failedMessagesReconcileAndRetryWithoutLosingDrafts()
{
    QTemporaryDir directory;const auto program=directory.filePath("hgs");QFile script(program);QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent;args=sys.argv[1:]
if args[0]=='inspect':print((root/'inspect.json').read_text())
elif args[0]=='send':
 p=json.load(sys.stdin)
 with (root/'requests.jsonl').open('a') as f:f.write(json.dumps(p)+'\n')
 time.sleep(.15)
 if not (root/'succeed').exists():print('terminal not ready',file=sys.stderr);sys.exit(1)
 print(json.dumps({'status':'submitted','request_id':p['request_id'],'name':args[1],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'submitted_text':p['text'],'submitted_at':time.time()}))
)PY");script.close();QVERIFY(FakeCli::makeRunnable(script.fileName()));
    QJsonObject details{{"tracked",true},{"run_id","run-ready"},{"conversation_id","conversation-ready"},{"runtime_state","live"},{"process_state","running"},{"activity","idle"},{"phase","idle"},{"events",QJsonArray{}},{"cursor",0}};
    // Atomic: a background inspection must never read a truncated file and reset the run identity a retry checks.
    const auto save=[&]{return FakeCli::replace(directory.filePath("inspect.json"),QJsonDocument(details).toJson());};QVERIFY(save());
    SessionsWindow window(FakeCli::program(program));window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *send=composer->findChild<QPushButton *>("sendMessage");
    auto *client=window.findChild<HgsClient *>();auto *view=composer->parentWidget()->findChild<ActivityView *>();QVERIFY(view);
    QSignalSpy failures(client,&HgsClient::messageFailed),sent(client,&HgsClient::messageSent);
    setComposerText(composer, "Original request");QTRY_VERIFY(send->isEnabled());send->click();QTRY_COMPARE(failures.size(),1);
    QTRY_VERIFY(view->browser()->toPlainText().contains("Not sent"));
    setComposerText(composer, "Keep this different draft");
    const auto now=QDateTime::currentMSecsSinceEpoch()/1000.;
    QJsonArray events{QJsonObject{{"seq",1},{"type","TurnStarted"},{"detail","Original request"},{"at",now-600}},
        QJsonObject{{"seq",2},{"type","TurnStarted"},{"detail","Original request"},{"at",now+1},{"agent_id","child"}}};
    details["events"]=events;details["cursor"]=2;QVERIFY(save());client->inspectionReady({},"codex/hgs/dashboard",details);
    QTRY_VERIFY(view->browser()->toPlainText().contains("Not sent"));
    events.append(QJsonObject{{"seq",3},{"type","TurnStarted"},{"detail","Original request"},{"at",now+2}});
    details["events"]=events;details["cursor"]=3;QVERIFY(save());client->inspectionReady({},"codex/hgs/dashboard",details);
    QTRY_VERIFY(!view->browser()->toPlainText().contains("Not sent"));QCOMPARE(composer->editor()->toPlainText(),QString("Keep this different draft"));
    setComposerText(composer, "Retry with a file");QVERIFY(composer->addAttachment("original.txt","text/plain","original"));send->click();QTRY_COMPARE(failures.size(),2);
    const auto id=QString::number(failures[1][0].toULongLong());QTRY_VERIFY(view->browser()->toPlainText().contains("Not sent"));
    setComposerText(composer, "Next draft");composer->findChild<QPushButton *>("removeAttachment")->click();QVERIFY(composer->addAttachment("next.txt","text/plain","next"));
    QFile success(directory.filePath("succeed"));QVERIFY(success.open(QIODevice::WriteOnly));success.close();
    {
        QSemaphore entered,release;std::atomic<int> reads=0;
        AttachmentFiles::setIoObserver([&]{if(++reads==1){entered.release();release.acquire();}});
        auto unblock=qScopeGuard([&]{release.release();AttachmentFiles::setIoObserver({});AttachmentFiles::pool().waitForDone();});
        view->messageActionRequested(id,"retry");view->messageActionRequested(id,"retry");QTRY_COMPARE(entered.available(),1);
        QCOMPARE(sent.size(),0);bool heartbeat=false;QTimer::singleShot(0,&window,[&]{heartbeat=true;});QTRY_VERIFY(heartbeat);
        window.showSession({},"kimi/docs/research");setComposerText(composer,"Other session draft");view->messageActionRequested(id,"retry");
        QCOMPARE(sent.size(),0);release.release();QTRY_COMPARE(sent.size(),1);
    }
    QCOMPARE(composer->editor()->toPlainText(),QString("Other session draft"));
    window.showSession({},"codex/hgs/dashboard");
    QTRY_COMPARE(composer->editor()->toPlainText(),QString("[File #2] Next draft"));
    QVERIFY(composer->draftMatches(window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString(),"[File #2] Next draft",{{"next.txt","text/plain","next","[File #2]"}}));
    QFile calls(directory.filePath("requests.jsonl"));QVERIFY(calls.open(QIODevice::ReadOnly));const auto requests=calls.readAll().trimmed().split('\n');QCOMPARE(requests.size(),3);
    const auto retried=QJsonDocument::fromJson(requests.last()).object();QCOMPARE(retried["text"].toString(),QString("[File #1] Retry with a file"));
    QCOMPARE(retried["attachments"].toArray()[0].toObject()["reference"].toString(),QString("[File #1]"));
    QCOMPARE(QByteArray::fromBase64(retried["attachments"].toArray()[0].toObject()["data_base64"].toString().toLatin1()),QByteArray("original"));
    QTRY_VERIFY(!view->browser()->toPlainText().contains("Not sent"));
    QFile::remove(directory.filePath("succeed"));QTRY_VERIFY(send->isEnabled());send->click();QTRY_COMPARE(failures.size(),3);
    const auto deletedId=QString::number(failures[2][0].toULongLong());
    {
        QSemaphore entered,release;AttachmentFiles::setIoObserver([&]{entered.release();release.acquire();});
        auto unblock=qScopeGuard([&]{release.release();AttachmentFiles::setIoObserver({});AttachmentFiles::pool().waitForDone();});
        view->messageActionRequested(deletedId,"retry");QTRY_COMPARE(entered.available(),1);
        view->messageActionRequested(deletedId,"delete");release.release();AttachmentFiles::pool().waitForDone();QTest::qWait(100);
        QCOMPARE(failures.size(),3);QCOMPARE(sent.size(),1); // A removed receipt cannot replay after the attachment read.
        QFile log(directory.filePath("requests.jsonl"));QVERIFY(log.open(QIODevice::ReadOnly));QCOMPARE(log.readAll().trimmed().split('\n').size(),4);
    }
    QTRY_VERIFY(!view->browser()->toPlainText().contains("Not sent"));QCOMPARE(composer->editor()->toPlainText(),QString("[File #2] Next draft"));
    composer->findChild<QPushButton *>("removeAttachment")->click();setComposerText(composer, "Sent through Terminal");
    send->click();QTRY_COMPARE(failures.size(),4);
    events.append(QJsonObject{{"seq",4},{"type","TurnStarted"},{"detail","Sent through Terminal"},{"at",QDateTime::currentMSecsSinceEpoch()/1000.+2}});
    details["events"]=events;details["cursor"]=4;QVERIFY(save());client->inspectionReady({},"codex/hgs/dashboard",details);
    QTRY_VERIFY(!view->browser()->toPlainText().contains("Not sent"));QVERIFY(composer->editor()->toPlainText().isEmpty());
}

void TestSessionsWindow::providerErrorWaitsForNativePrompt_data()
{
    QTest::addColumn<QString>("state");
    QTest::newRow("provider-error")<<QString("error");
    QTest::newRow("interrupted")<<QString("interrupted");
}
void TestSessionsWindow::providerErrorWaitsForNativePrompt()
{
    QFETCH(QString,state);
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>();
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *send = composer->findChild<QPushButton *>("sendMessage");
    auto *status = composer->findChild<QLabel *>("messageStatus");
    QTRY_VERIFY(status->text().contains("starting"));
    setComposerText(composer, "повтори");
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"activity", state=="error"?"attention":"unknown"}, {"phase", state},
        {"events", QJsonArray{}}, {"cursor", 0}};
    if(state=="error")details["provider_error"]=QJsonObject{{"error_kind","capacity"}};
    const auto update = [&] { client->inspectionReady({}, "codex/hgs/dashboard", details, {}); };
    update(); QVERIFY(!send->isEnabled()); // Older/absent evidence never enables retry.
    details[state+"_message_can_send"] = false; details[state+"_message_reason"] = "Terminal has a draft";
    update(); QVERIFY(!send->isEnabled()); QCOMPARE(status->text(), QString("Terminal has a draft"));
    details[state+"_message_can_send"] = true; details[state+"_message_reason"] = "";
    update(); QVERIFY(send->isEnabled()); QCOMPARE(composer->editor()->toPlainText(), QString("повтори"));
    for (const QString &phase : {QString("approval"), QString("input"), QString("unknown")}) {
        details["phase"] = phase; update(); QVERIFY(!send->isEnabled());
    }
    details["phase"] = state; details["error"] = "Tracking mismatch"; update(); QVERIFY(!send->isEnabled());
    details.remove("error"); details["process_state"] = "exited"; update(); QVERIFY(!send->isEnabled());
    QCOMPARE(composer->editor()->toPlainText(), QString("повтори"));
}

void TestSessionsWindow::quotaFailureExplainsRecoveryAndKeepsDraft()
{
    auto state=fleet();auto box=state.local();box.sessions[0].runId="run-one";box.sessions[0].conversationId="conversation-one";state.setLocal(box,0);
    SessionsWindow window(program());window.setFleet(state);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();auto *composer=window.findChild<MessageComposer *>("messageComposer");
    QTRY_VERIFY(composer->findChild<QLabel *>("messageStatus")->text().contains("starting"));
    setComposerText(composer, "Continue after account access is restored");
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"last_event_at",2000000000},{"runtime_state","live"},{"process_state","running"},
        {"activity","attention"},{"phase","error"},{"error_message_can_send",true},
        {"provider_error",QJsonObject{{"error_kind","quota"},{"detail","7d limit reached. Resets in 4d18h"}}},
        {"events",QJsonArray{}},{"cursor",0}};
    client->inspectionReady({},"codex/hgs/dashboard",details,{});
    auto *chip=window.findChild<ToolbarChip *>("recoveryChip");QVERIFY(chip && chip->isVisible());
    QCOMPARE(chip->fullLabel(),QString("Provider error"));QCOMPARE(chip->tone(),ChipTone::Danger);
    auto *panel=window.findChild<QFrame *>("recoveryPanel");QVERIFY(panel && !panel->isVisible());
    window.activateWindow();QTest::mouseClick(chip,Qt::LeftButton);QTRY_VERIFY(panel->isVisible());
    const auto *row=window.findChild<SessionList *>("sessionList")->currentItem();
    QCOMPARE(row->data(SessionRoles::Status).toString(),QString("Usage limit reached"));
    QVERIFY(!row->data(SessionRoles::Working).toBool());QVERIFY(row->data(SessionRoles::Attention).toBool());
    QVERIFY(panel->findChild<QLabel *>("providerErrorHelp")->text().contains("will not retry automatically"));
    QVERIFY(panel->findChild<QPushButton *>("providerErrorTerminal")->isVisible());
    QVERIFY(panel->findChild<QPushButton *>("providerErrorRefresh")->isVisible());
    auto *action=panel->findChild<QPushButton *>("providerErrorTerminal");action->setFocus();
    QTRY_COMPARE(QApplication::focusWidget(),action);
    client->inspectionReady({},"codex/hgs/dashboard",details,{});
    QCOMPARE(QApplication::focusWidget(),action);
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue after account access is restored"));
    const auto preview=qEnvironmentVariable("HGS_QUOTA_PREVIEW");
    if(!preview.isEmpty()) {QDir().mkpath(preview);window.resize(1120,800);QTest::qWait(30);QVERIFY(window.grab().save(preview+"/quota-failure.png"));}
    // Switching session closes the popover instead of showing this session's error elsewhere.
    window.showSession("mac","claude/infra/review");QVERIFY(!panel->isVisible());window.showSession({},"codex/hgs/dashboard");
    // Restored native progress clears the failure without submitting the draft.
    details["phase"]="working";details["activity"]="busy";details.remove("provider_error");
    client->inspectionReady({},"codex/hgs/dashboard",details,{});
    QVERIFY(!panel->isVisible());QVERIFY(chip->isHidden());
    QCOMPARE(composer->editor()->toPlainText(),QString("Continue after account access is restored"));
}

void TestSessionsWindow::firstMessageWaitsForNativePrompt_data()
{
    QTest::addColumn<QString>("startup");
    QTest::newRow("new") << QString("new");
    QTest::newRow("resumed") << QString("resumed");
    QTest::newRow("forked") << QString("forked");
}

void TestSessionsWindow::firstMessageWaitsForNativePrompt()
{
    QFETCH(QString, startup);
    const bool resumed = startup == "resumed";
    QFile::remove(m_dir->filePath("first-payload.json"));
    const QString program = m_dir->filePath("first-message-hgs");
    QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"PY(#!/usr/bin/env python3
import sys,json,pathlib
root=pathlib.Path(__file__).parent
args=sys.argv[1:]
if args[0]=='inspect': print((root/'first-state.json').read_text())
elif args[0]=='send':
 p=json.load(sys.stdin)
 (root/'first-payload.json').write_text(json.dumps(p))
 print(json.dumps({'status':'submitted','request_id':p['request_id'],'name':args[1],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'first_message':not p['expected_conversation_id'],'submitted_text':p['text']}))
)PY");
    file.close(); QVERIFY(FakeCli::makeRunnable(file.fileName()));
    QJsonObject state{{"tracked", true}, {"run_id", "fresh-run"}, {"conversation_id", ""},
        {"runtime_state", "live"}, {"process_state", "running"}, {"activity", "unknown"}, {"phase", "unknown"},
        {"first_message_can_send", false}, {"first_message_reason", "Finish login in Terminal"}, {"events", QJsonArray{}}, {"cursor", 0}};
    if (startup == "forked") {
        state["fork_parent_id"] = "parent-conversation";
        state["fork_source_name"] = "codex/hgs/source";
    }
    if (resumed) {
        state["conversation_id"] = "saved-conversation"; state["expected_id"] = "saved-conversation";
        state["resume_message_can_send"] = false; state["resume_message_reason"] = "Finish login in Terminal";
    }
    // Polls read first-state.json while it changes: replace it atomically, or a torn
    // read fails an inspection and the session refuses to send.
    const auto save = [&] { return FakeCli::replace(m_dir->filePath("first-state.json"), QJsonDocument(state).toJson()); };
    QVERIFY(save());
    SessionsWindow window(FakeCli::program(program)); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *send = window.findChild<MessageComposer *>("messageComposer")->findChild<QPushButton *>("sendMessage"); auto *status = window.findChild<MessageComposer *>("messageComposer")->findChild<QLabel *>("messageStatus");
    QTRY_COMPARE(status->text(), QString("Finish login in Terminal"));
    setComposerText(composer, "First message after login"); QVERIFY(composer->addAttachment("notes.txt", "text/plain", "keep attachment"));
    QVERIFY(!send->isEnabled()); QTest::keyClick(composer->editor(), Qt::Key_Return);
    QVERIFY(!QFile::exists(m_dir->filePath("first-payload.json")));
    state["first_message_can_send"] = true; state["first_message_reason"] = ""; state["model"] = "gpt-6-astra"; state["effort"] = "xhigh"; QVERIFY(save());
    if (resumed) { state["resume_message_can_send"] = true; state["resume_message_reason"] = ""; QVERIFY(save()); }
    client->requestInspection({}, "codex/hgs/dashboard", 0); QTRY_VERIFY(send->isEnabled());
    QVERIFY(window.findChild<MessageComposer *>("messageComposer")->findChild<QPushButton *>("sessionModelSettings")->text().contains("gpt-6-astra"));
    QSignalSpy delivered(client, &HgsClient::messageSent); QTest::keyClick(composer->editor(), Qt::Key_Return); QTRY_COMPARE(delivered.size(), 1);
    QFile captured(m_dir->filePath("first-payload.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto payload = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(payload["expected_run_id"].toString(), "fresh-run");
    QCOMPARE(payload["expected_conversation_id"].toString(), resumed ? "saved-conversation" : "");
    QCOMPARE(payload["text"].toString(), "[File #1] First message after login"); QCOMPARE(payload["attachments"].toArray().size(), 1);
    QCOMPARE(composer->editor()->toPlainText(), QString());
    state.remove("expected_id"); state.remove("resume_message_can_send");
    state["conversation_id"] = resumed ? "saved-conversation" : "confirmed-first";
    state["activity"] = "busy"; state["phase"] = "working"; QVERIFY(save());
    client->requestInspection({}, "codex/hgs/dashboard", 0);
    setComposerText(composer, "Next message"); QTRY_VERIFY(send->isEnabled());
}

void TestSessionsWindow::resumedConversationPausesAndClearsOnceVerified()
{
    // Codex defers SessionStart after resume, so the binding still expects the
    // saved conversation. Only hgs's verification enables Pause and Clear.
    const QString program = m_dir->filePath("resumed-actions-hgs");
    QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"PY(#!/usr/bin/env python3
import sys,pathlib
root=pathlib.Path(__file__).parent
print((root/'resumed-state.json').read_text() if sys.argv[1:2]==['inspect'] else '{}')
)PY");
    file.close(); QVERIFY(FakeCli::makeRunnable(file.fileName()));
    QJsonObject state{{"tracked", true}, {"run_id", "resumed-run"}, {"conversation_id", "saved-conversation"},
        {"expected_id", "saved-conversation"}, {"runtime_state", "live"}, {"process_state", "running"},
        {"activity", "unknown"}, {"phase", "starting"}, {"resume_message_can_send", false},
        {"resume_message_reason", "Codex has not opened the saved conversation yet"}, {"clear_context_supported", false},
        {"allowed_actions", QJsonArray{"rename", "terminate", "forget"}}, {"events", QJsonArray{}}, {"cursor", 0}};
    const auto save = [&] { return FakeCli::replace(m_dir->filePath("resumed-state.json"), QJsonDocument(state).toJson()); };
    QVERIFY(save());
    SessionsWindow window(FakeCli::program(program)); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *pause = window.findChild<QAction *>("pauseAction"); auto *clear = window.findChild<QPushButton *>("clearSessionAction");
    settleSelectedInspection(composer, client, {}, "codex/hgs/dashboard", state);
    QVERIFY(!pause->isEnabled()); QVERIFY(!clear->isEnabled());
    // Verified: the exact process holds the saved history at an empty composer.
    // An hgs that reports this without allowing the pause keeps it disabled.
    state["activity"] = "idle"; state["phase"] = "idle"; state["resume_message_can_send"] = true; state["resume_message_reason"] = "";
    QVERIFY(save()); settleSelectedInspection(composer, client, {}, "codex/hgs/dashboard", state);
    QVERIFY(!pause->isEnabled()); QVERIFY(!clear->isEnabled());
    state["clear_context_supported"] = true; state["allowed_actions"] = QJsonArray{"pause", "rename", "terminate", "forget"};
    QVERIFY(save()); settleSelectedInspection(composer, client, {}, "codex/hgs/dashboard", state);
    QVERIFY(pause->isEnabled()); QVERIFY(clear->isEnabled());
}

void TestSessionsWindow::coldQuestionSubmissionRevalidatesConsent_data()
{
    QTest::addColumn<QString>("change");
    QTest::addColumn<bool>("sent");
    for (const auto *name : {"cancel", "escape", "close", "run", "conversation", "question-id", "hash",
            "schema", "source", "tool", "tool-call", "approval-identity", "capability", "offline", "selection", "entry-identity", "raw-draft", "page", "sending"})
        QTest::newRow(name) << QString::fromLatin1(name) << false;
    for (const auto *name : {"proceed", "benign-poll", "duplicate", "warm", "ttl", "skip", "approval", "broader-approval"})
        QTest::newRow(name) << QString::fromLatin1(name) << true;
}

void TestSessionsWindow::coldQuestionSubmissionRevalidatesConsent()
{
    QFETCH(QString, change); QFETCH(bool, sent);
    QTemporaryDir directory;
    const auto program = directory.filePath("hgs"), state = directory.filePath("details.json"), capture = directory.filePath("answer.json");
    QFile fixture(program); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
if sys.argv[1]=='inspect': print((root/'details.json').read_text())
elif sys.argv[1]=='answer':
 p=json.load(sys.stdin)
 with (root/'calls').open('a') as f:f.write('call\n')
 staged=root/'answer.tmp';staged.write_text(json.dumps({'args':sys.argv[1:],'payload':p}));staged.replace(root/'answer.json')
 time.sleep(.1)
 print(json.dumps(dict(status='answered',request_id=p['request_id'],name=sys.argv[2],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],question_id=p['question_id'],question_hash=p['expected_question_hash'])))
else: print('{}')
)PY");
    fixture.close(); QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    const bool approval = change == "approval" || change == "broader-approval";
    QJsonArray prompts;
    for (int index = 0; index < (approval ? 1 : 2); ++index)
        prompts.append(QJsonObject{{"id", QString("q_%1").arg(index)}, {"header", "Scope"}, {"question", "Which area?"},
            {"multi_select", false}, {"allow_other", !approval}, {"options", QJsonArray{
                QJsonObject{{"id", approval ? "allow" : "documents"}, {"label", approval ? "Allow once" : "Documents"}},
                QJsonObject{{"id", approval ? "deny" : "source"}, {"label", approval ? "Always allow" : "Source"}, {"scope", approval ? "broader" : "request"}}}}});
    QJsonObject question{{"question_id", "interaction-cold"}, {"question_hash", "hash-cold"}, {"run_id", "run-cold"},
        {"conversation_id", "conversation-cold"}, {"can_answer", true}, {"can_skip", true}, {"questions", prompts},
        {"source", change == "skip" ? "codex_async" : "claude"}, {"tool", "AskUserQuestion"}, {"tool_call_id", "toolu-cold"}, {"approval", approval},
        {"approval_choices", change == "broader-approval"}, {"optional", change == "skip"}};
    if (change == "skip") question["can_answer"] = false;
    QJsonObject details{{"tracked", true}, {"run_id", "run-cold"}, {"conversation_id", "conversation-cold"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"phase", "input"}, {"activity", "busy"},
        {"pending_questions", QJsonArray{question}}, {"cache_hint", QJsonObject{{"status", "cold"}, {"tokens", 12000}}},
        {"compact_context_supported", true}, {"clear_context_supported", true}};
    const qint64 expiry = QDateTime::currentSecsSinceEpoch() + (change == "ttl" ? 1 : 600);
    if (change == "warm" || change == "ttl") {
        details.remove("cache_hint");
        details["session_usage"] = QJsonObject{{"prompt_cache", QJsonObject{{"status", "warm"}, {"expires_at", expiry}}}};
    }
    const auto writeDetails = [&] { QVERIFY(FakeCli::replace(state, QJsonDocument(details).toJson())); };
    writeDetails();
    SessionsWindow window(FakeCli::program(program)); window.resize(1080, 760); window.setFleet(fleet());
    keepDetailsPollsOut(window); window.show(); window.showSession({}, "kimi/docs/research");
    auto *client = window.findChild<HgsClient *>(); auto *card = window.findChild<QuestionCard *>();
    QTRY_VERIFY(card->isVisible()); QTRY_VERIFY(!card->isDraftLoading());
    // A change made while the confirmation is open must stay the latest evidence. An
    // inspection that read details.json before it would restore the reviewed identity,
    // on Windows even before the accepted confirmation returns, and the answer would be
    // sent. Settle one here, so none is running when the confirmation opens.
    settleSelectedInspection(window.findChild<MessageComposer *>("messageComposer"), client, {}, "kimi/docs/research", details);
    auto *list = window.findChild<QListWidget *>("sessionList");
    const QString key = list->currentItem()->data(Qt::UserRole).toString();
    auto *tabs = card->findChild<QTabBar *>("questionTabs");
    if (!approval) {
        for (auto *button : card->findChildren<QAbstractButton *>("questionOther")) button->click();
        for (auto *editor : card->findChildren<QLineEdit *>("questionFreeText")) editor->setText("  Exact  draft  ");
        tabs->setCurrentIndex(1);
    } else if (change == "broader-approval") {
        for (auto *button : card->findChildren<QAbstractButton *>("questionOption"))
            if (button->property("optionId") == "deny") button->click();
    }
    auto *action = card->findChild<QPushButton *>(change == "skip" ? "skipQuestion" : change == "approval" ? "denyToolRequest" : "submitQuestionAnswer");
    if (change == "broader-approval") { card->setReviewDelayForTests(200); window.activateWindow(); QTRY_VERIFY(action->isEnabled()); }
    QVERIFY(action->isEnabled());
    const auto before = card->submissionSnapshot(key, "interaction-cold");
    QJsonArray requested;
    QSignalSpy answerSignal(card, &QuestionCard::answerRequested);
    QSignalSpy completed(client, &HgsClient::questionAnswered);
    QSignalSpy failed(client, &HgsClient::questionAnswerFailed);
    QSignalSpy submitted(client, &HgsClient::questionAnswerSubmitted);
    bool shown = false;
    QTimer modal;
    modal.setInterval(5);
    connect(&modal, &QTimer::timeout, &window, [&] {
        auto *box = window.findChild<QMessageBox *>("questionColdCacheConfirm");
        if (!box || !box->isVisible()) return;
        modal.stop(); shown = true;
        QCOMPARE(box->buttons().size(), 2);
        QCOMPARE(box->defaultButton(), qobject_cast<QPushButton *>(box->button(QMessageBox::Cancel)));
        QVERIFY(box->text().contains("Compaction or clearing can invalidate"));
        QVERIFY(!QFileInfo::exists(capture));
        QVERIFY(!card->submissionSnapshot(key, "interaction-cold").value("sending").toBool());
        if (change == "cancel") { box->button(QMessageBox::Cancel)->click(); return; }
        if (change == "escape") { QTest::keyClick(box, Qt::Key_Escape); return; }
        if (change == "close") { box->close(); return; }
        if (change == "duplicate") card->answerRequested(key, "interaction-cold", QJsonArray{QJsonObject{{"question_id", "q_0"}, {"selected_option_ids", QJsonArray{}}, {"text", "Exact  draft"}}, QJsonObject{{"question_id", "q_1"}, {"selected_option_ids", QJsonArray{}}, {"text", "Exact  draft"}}});
        if (change == "selection") window.showSession({}, "codex/hgs/dashboard");
        else if (change == "offline" || change == "entry-identity") {
            auto boxes = fleet(); auto boxState = boxes.local();
            for (auto &session : boxState.sessions)
                if (session.name == "kimi/docs/research") { if (change == "offline") boxState.ok = false; else session.created += 1; }
            boxes.setLocal(boxState, QDateTime::currentMSecsSinceEpoch()); window.setFleet(boxes);
        } else if (change == "raw-draft") card->findChildren<QLineEdit *>("questionFreeText").first()->setText(" Exact  draft  ");
        else if (change == "page") tabs->setCurrentIndex(0);
        else if (change == "sending") card->setSending(key, "interaction-cold");
        else {
            if (change == "run") { details["run_id"] = "run-new"; question["run_id"] = "run-new"; }
            if (change == "conversation") { details["conversation_id"] = "conversation-new"; question["conversation_id"] = "conversation-new"; }
            if (change == "question-id") question["question_id"] = "interaction-new";
            if (change == "hash") question["question_hash"] = "hash-new";
            if (change == "schema") { auto prompt = prompts.first().toObject(); prompt["question"] = "Changed disclosure"; prompts[0] = prompt; question["questions"] = prompts; }
            if (change == "source") question["source"] = "changed-source";
            if (change == "tool") question["tool"] = "changed-tool";
            if (change == "tool-call") question["tool_call_id"] = "toolu-new";
            if (change == "approval-identity") question["approval"] = true;
            if (change == "benign-poll") { question["created_at"] = 42; question["answer_unavailable_reason"] = "Updated evidence"; details["last_event_at"] = 43; }
            if (change == "capability") question["can_answer"] = false;
            details["pending_questions"] = QJsonArray{question};
            writeDetails(); client->inspectionReady({}, "kimi/docs/research", details);
        }
        for (auto *button : box->buttons()) if (box->buttonRole(button) == QMessageBox::AcceptRole) button->click();
    });
    // No inspection is needed for click-time TTL expiry (whole seconds, as CacheStatus compares).
    if (change == "ttl") QTRY_VERIFY(QDateTime::currentSecsSinceEpoch() >= expiry);
    modal.start();
    if (change == "broader-approval") { action->click(); QVERIFY(!shown); QVERIFY(!QFileInfo::exists(capture)); QCOMPARE(action->text(), QString("Confirm")); }
    action->click();
    modal.stop();
    QCOMPARE(shown, change != "warm");
    QVERIFY(!answerSignal.isEmpty()); requested = answerSignal.first().at(2).value<QJsonArray>();
    if (!sent) {
        // An erroneous QProcess dispatch can write after the modal returns.
        // Drain the fixture's completion interval and watch all answer outcomes.
        QTest::qWait(300);
        QVERIFY(!QFileInfo::exists(capture));
        QVERIFY(!QFileInfo::exists(directory.filePath("calls")));
        QCOMPARE(completed.size(), 0); QCOMPARE(failed.size(), 0); QCOMPARE(submitted.size(), 0);
        if (change == "cancel" || change == "escape" || change == "close") QCOMPARE(card->submissionSnapshot(key, "interaction-cold"), before);
        if (change == "raw-draft") QCOMPARE(card->findChildren<QLineEdit *>("questionFreeText").first()->text(), QString(" Exact  draft  "));
        return;
    }
    card->answerRequested(key, "interaction-cold", requested); // In-flight repeats create no second request or modal.
    QTRY_VERIFY(QFileInfo::exists(capture));
    QFile result(capture); QVERIFY(result.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(result.readAll()).object();
    QCOMPARE(record["args"].toArray(), QJsonArray({"answer", "kimi/docs/research", "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["answers"].toArray(), requested);
    QCOMPARE(payload["question_id"].toString(), QString("interaction-cold"));
    QCOMPARE(payload["expected_question_hash"].toString(), QString("hash-cold"));
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-cold"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-cold"));
    QTRY_COMPARE(completed.size(), 1);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestSessionsWindow::nativeColdCacheChoiceKeepsExactAnswer_data()
{
    QTest::addColumn<int>("choice"); QTest::addColumn<QString>("change");
    for (int choice = 0; choice < 4; ++choice)
        QTest::newRow(qPrintable(QString("choice-%1").arg(choice))) << choice << QString();
    for (const auto *change : {"hash", "run", "source"}) QTest::newRow(change) << 0 << QString(change);
}

void TestSessionsWindow::nativeColdCacheChoiceKeepsExactAnswer()
{
    QFETCH(int, choice); QFETCH(QString, change);
    QTemporaryDir directory;
    const auto program = directory.filePath("hgs"), state = directory.filePath("details.json"), capture = directory.filePath("answer.json");
    QFile fixture(program); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
if sys.argv[1]=='inspect': print((root/'details.json').read_text())
elif sys.argv[1]=='answer':
 p=json.load(sys.stdin)
 with (root/'calls').open('a') as f:f.write('call\n')
 (root/'answer.json').write_text(json.dumps(p))
 time.sleep(.1)
 print(json.dumps(dict(status='answered',request_id=p['request_id'],name=sys.argv[2],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],question_id=p['question_id'],question_hash=p['expected_question_hash'])))
else: print('{}')
)PY");
    fixture.close(); QVERIFY(FakeCli::makeRunnable(program));
    QJsonArray options;
    const QStringList labels{"Compact and continue", "Start a new session", "Continue as-is", "Don't ask me again"};
    for (int index = 0; index < labels.size(); ++index)
        options.append(QJsonObject{{"id", QString("cache_%1").arg(index)}, {"label", labels[index]}});
    QJsonObject question{{"question_id", "kimi-cache:exact"}, {"question_hash", "hash-cold"}, {"run_id", "run-cold"},
        {"conversation_id", "conversation-cold"}, {"can_answer", true}, {"source", "kimi_cache_hint"},
        {"answer_transport", "kimi_tui"}, {"tool_call_id", "kimi_cache_hint"}, {"agent_id", "main"},
        {"questions", QJsonArray{QJsonObject{{"id", "cache"}, {"header", "Context"}, {"question", "Cache expired"},
            {"body", "The next message re-sends the entire history at full price."},
            {"multi_select", false}, {"allow_other", false}, {"options", options}}}}};
    QJsonObject details{{"tracked", true}, {"run_id", "run-cold"}, {"conversation_id", "conversation-cold"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"phase", "input"}, {"activity", "busy"},
        {"pending_questions", QJsonArray{question}}, {"cache_hint", QJsonObject{{"status", "cold"}, {"tokens", 490252}}}};
    QVERIFY(FakeCli::replace(state, QJsonDocument(details).toJson()));
    SessionsWindow window(FakeCli::program(program)); window.setFleet(fleet()); keepDetailsPollsOut(window); window.show(); window.showSession({}, "kimi/docs/research");
    auto *card = window.findChild<QuestionCard *>(); auto *client = window.findChild<HgsClient *>();
    QTRY_VERIFY(card->isVisible()); QTRY_VERIFY(!card->isDraftLoading());
    // A change made while the answer's draft is saved must stay the latest evidence.
    // An inspection that read details.json before it would restore the reviewed
    // question, and the held answer would be sent. Settle one before submitting.
    settleSelectedInspection(window.findChild<MessageComposer *>("messageComposer"), client, {}, "kimi/docs/research", details);
    const auto key = window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString();
    for (auto *button : card->findChildren<QAbstractButton *>("questionOption"))
        if (button->property("optionId") == QString("cache_%1").arg(choice)) button->click();
    auto *submit = card->findChild<QPushButton *>("submitQuestionAnswer"); QVERIFY(submit->isEnabled());
    ComposerDraftStore::flushWrites();
    QSemaphore entered, release;
    if (!change.isEmpty()) ComposerDraftStore::setWriteObserver([&] { entered.release(); release.acquire(); });
    auto unblock = qScopeGuard([&] { release.release(10); ComposerDraftStore::flushWrites(); ComposerDraftStore::setWriteObserver({}); });
    bool unwantedConfirmation = false;
    QTimer modal; modal.setInterval(5);
    connect(&modal, &QTimer::timeout, &window, [&] {
        if (auto *box = window.findChild<QMessageBox *>("questionColdCacheConfirm")) {
            unwantedConfirmation = true; box->reject();
        }
    });
    QSignalSpy completed(client, &HgsClient::questionAnswered);
    modal.start(); submit->click(); QVERIFY(!unwantedConfirmation); modal.stop();
    if (!change.isEmpty()) {
        QTRY_VERIFY(entered.available() > 0); entered.acquire();
        ComposerDraftStore::setWriteObserver({});
        if (change == "hash") question["question_hash"] = "changed";
        if (change == "source") question["source"] = "unreviewed";
        if (change == "run") { question["run_id"] = "changed"; details["run_id"] = "changed"; }
        details["pending_questions"] = QJsonArray{question};
        QVERIFY(FakeCli::replace(state, QJsonDocument(details).toJson()));
        client->inspectionReady({}, "kimi/docs/research", details, {});
        release.release(10); ComposerDraftStore::flushWrites(); QTest::qWait(200);
        QVERIFY(!QFileInfo::exists(capture)); QCOMPARE(completed.size(), 0); return;
    }
    card->answerRequested(key, "kimi-cache:exact", QJsonArray{}); // No duplicate delivery during the save barrier.
    QTRY_COMPARE(completed.size(), 1);
    QFile result(capture); QVERIFY(result.open(QIODevice::ReadOnly));
    const auto payload = QJsonDocument::fromJson(result.readAll()).object();
    QCOMPARE(payload["question_id"].toString(), QString("kimi-cache:exact"));
    QCOMPARE(payload["expected_question_hash"].toString(), QString("hash-cold"));
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-cold"));
    QCOMPARE(payload["expected_conversation_id"].toString(), QString("conversation-cold"));
    const auto answer = payload["answers"].toArray().first().toObject();
    QCOMPARE(answer["question_id"].toString(), QString("cache"));
    QCOMPARE(answer["selected_option_ids"].toArray(), QJsonArray{QString("cache_%1").arg(choice)});
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestSessionsWindow::questionAnswersStayWithOriginalSession()
{
    QTemporaryDir directory;
    const QString program = directory.filePath("hgs");
    QFile file(program); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
args=sys.argv[1:]
root=pathlib.Path(__file__).parent
question={'question_id':'interaction-one','question_hash':'exact-hash','run_id':'run-one','conversation_id':'conv-one','can_answer':True,
 'questions':[{'id':'q_0','header':'Scope','question':'Which area should I inspect?','multi_select':False,'allow_other':True,
 'options':[{'id':'opt_0_0','label':'Documents','description':'Read the project docs'},{'id':'opt_0_1','label':'Source','description':'Read source files'}]}]}
if args[0]=='inspect':
 pending=args[1]=='kimi/docs/research' and not (root/'done').exists()
 print(json.dumps({'tracked':True,'run_id':'run-one','conversation_id':'conv-one','runtime_state':'live','process_state':'running',
 'activity':'busy' if pending else 'idle','phase':'input' if pending else 'idle','last_event_at':2000000000,
 'pending_questions':[question] if pending else [],'events':[],'cursor':0}))
elif args[0]=='answer':
 payload=json.load(sys.stdin)
 (root/'answer.json').write_text(json.dumps({'args':args,'payload':payload}))
 with (root/'calls').open('a') as f:f.write('call\n')
 time.sleep(.15)
 (root/'done').touch()
 print(json.dumps({'status':'answered','request_id':payload['request_id'],'name':args[1],
 'run_id':payload['expected_run_id'],'conversation_id':payload['expected_conversation_id'],
 'question_id':payload['question_id'],'question_hash':payload['expected_question_hash']}))
)PY");
    file.close(); QVERIFY(FakeCli::makeRunnable(file.fileName()));
    SessionsWindow window(FakeCli::program(program)); window.setFleet(fleet()); window.show(); window.showSession({}, "kimi/docs/research");
    auto *card = window.findChild<QuestionCard *>(); QVERIFY(card);
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); QVERIFY(composer);
    auto *client = window.findChild<HgsClient *>(); QVERIFY(client);
    auto *list = window.findChild<QListWidget *>("sessionList"); QVERIFY(list);
    QSignalSpy answered(client, &HgsClient::questionAnswered);
    QTRY_VERIFY(card->isVisible()); QTRY_VERIFY(!card->isDraftLoading()); QVERIFY(!composer->isInputVisible()); QVERIFY(composer->toolbar()->isVisible());
    if (!qEnvironmentVariable("HGS_PREVIEW_DIR").isEmpty()) {
        window.resize(1240, 900); QTest::qWait(30);
        QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PREVIEW_DIR") + "/sessions-question.png"));
    }
    const QString key = list->currentItem()->data(Qt::UserRole).toString();
    const QJsonArray choices{QJsonObject{{"question_id", "q_0"}, {"selected_option_ids", QJsonArray{}}, {"text", "Inspect source using Enter"}}};
    card->findChild<QAbstractButton *>("questionOther")->click();
    auto *answerEditor = card->findChild<QLineEdit *>("questionFreeText");
    answerEditor->setText("Inspect source using Enter"); answerEditor->setFocus();
    QTest::keyClick(answerEditor, Qt::Key_Return);
    QTest::keyClick(answerEditor, Qt::Key_Return); // an in-flight answer cannot be sent twice
    window.showSession({}, "codex/hgs/dashboard"); setComposerText(composer, "Keep this other-session draft");
    card->answerRequested(key, "interaction-one", choices); // stale selection cannot send again
    QTRY_COMPARE(answered.size(), 1);
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep this other-session draft"));
    QFile captured(directory.filePath("answer.json")); QVERIFY(captured.open(QIODevice::ReadOnly));
    const auto record = QJsonDocument::fromJson(captured.readAll()).object();
    QCOMPARE(record["args"].toArray(), QJsonArray({"answer", "kimi/docs/research", "--json"}));
    const auto payload = record["payload"].toObject();
    QCOMPARE(payload["answers"].toArray(), choices);
    QCOMPARE(payload["expected_question_hash"].toString(), QString("exact-hash"));
    QCOMPARE(payload["expected_run_id"].toString(), QString("run-one"));
    window.showSession({}, "kimi/docs/research");
    QTRY_VERIFY(!card->isVisible()); QVERIFY(composer->isInputVisible());
    card->answerRequested(key, "interaction-one", choices); // resolved question is no longer actionable
    QTest::qWait(30);
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestSessionsWindow::queuedQuestionAnswersDisappearAfterSubmission()
{
    QTemporaryDir directory;QFile program(directory.filePath("hgs"));QVERIFY(program.open(QIODevice::WriteOnly));
    // Polls read details.json while the answer rewrites it: replace it, so no inspection
    // reads a truncated file and fails. Windows refuses the rename while a poll has the
    // file open; retry then.
    program.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
if sys.argv[1]=='inspect':
 print((root/'details.json').read_text())
elif sys.argv[1]=='answer':
 p=json.load(sys.stdin)
 with (root/'calls').open('a') as f:f.write('call\n')
 details=json.loads((root/'details.json').read_text())
 q=details['pending_questions'][0]
 q['can_answer']=False;q['can_skip']=False
 q['answer_delivery']={'status':'submitted','request_id':p['request_id'],'run_id':p['expected_run_id'],
  'conversation_id':p['expected_conversation_id'],'question_id':p['question_id'],
  'question_hash':p['expected_question_hash'],'answers':p['answers']}
 details['input_queue']={'id':'native-queue-one','text':'↳ Which validation should run? → '+p['answers'][0]['text'],
  'can_send_now':True,'hint':'Interrupt and send the queued answer using the native action.'}
 staged=root/'details.tmp';staged.write_text(json.dumps(details))
 for _ in range(250):
  try:staged.replace(root/'details.json');break
  except PermissionError:time.sleep(.02)
 else:sys.exit('details.json stayed open')
 print(json.dumps(dict(q['answer_delivery'],name=sys.argv[2])))
)PY");program.close();QVERIFY(FakeCli::makeRunnable(program.fileName()));
    const QJsonObject question{{"question_id","optional-one"},{"question_hash","hash-one"},{"run_id","run-one"},
        {"conversation_id","conversation-one"},{"source","codex_async"},{"optional",true},{"can_answer",true},{"can_skip",true},
        {"questions",QJsonArray{QJsonObject{{"id","q_0"},{"question","Which validation should run?"},{"allow_other",true},{"options",QJsonArray{}}}}}};
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"runtime_state","live"},{"process_state","running"},{"activity","busy"},{"phase","thinking"},
        {"pending_questions",QJsonArray{question}},{"events",QJsonArray{}},{"cursor",0}};
    const auto save=[&] { return FakeCli::replace(directory.filePath("details.json"), QJsonDocument(details).toJson()); };QVERIFY(save());
    SessionsWindow window(FakeCli::program(program.fileName()));window.setFleet(fleet());window.show();window.activateWindow();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();auto *card=window.findChild<QuestionCard *>();
    auto *composer=window.findChild<MessageComposer *>("messageComposer");
    auto *activity=window.findChild<ActivityView *>("mainActivity");QVERIFY(activity);
    QSignalSpy promoted(activity,&ActivityView::queueSendNowRequested);
    QSignalSpy submitted(client,&HgsClient::questionAnswerSubmitted),answered(client,&HgsClient::questionAnswered);
    QSignalSpy messages(composer,&MessageComposer::sendRequested),stopped(composer,&MessageComposer::interruptRequested);
    QTRY_VERIFY(card->isVisible()); QTRY_VERIFY(!card->isDraftLoading());auto *editor=card->findChild<QLineEdit *>("questionFreeText");QVERIFY(editor);
    editor->setText("Run unit tests and a desktop preview.");setComposerText(composer, "Keep my next-message draft");
    auto *send=card->findChild<QPushButton *>("submitQuestionAnswer");QVERIFY(send->isEnabled());
    QVERIFY(composer->isInputVisible());editor->setFocus();QTRY_VERIFY(editor->hasFocus());QTest::keyClick(editor,Qt::Key_Return);
    QTRY_COMPARE(submitted.size(),1);QCOMPARE(answered.size(),0);QVERIFY(card->isHidden());
    QCOMPARE(messages.size(),0);QCOMPARE(stopped.size(),0);
    QVERIFY(composer->editor()->hasFocus());
    QFile fixture(directory.filePath("details.json"));QVERIFY(fixture.open(QIODevice::ReadOnly));
    details=QJsonDocument::fromJson(fixture.readAll()).object();fixture.close();
    client->inspectionReady({},"codex/hgs/dashboard",details);
    QVERIFY(card->isHidden());QCOMPARE(details["pending_questions"].toArray().size(),1);
    QVERIFY(activity->findChild<QWidget *>("activityInputQueue")->isVisible());
    QVERIFY(activity->findChild<QPushButton *>("queueSendNow")->isEnabled());QCOMPARE(promoted.size(),0);
    QVERIFY(composer->isInputVisible());QCOMPARE(composer->editor()->toPlainText(),QString("Keep my next-message draft"));
    auto stale=details;stale["pending_questions"]=QJsonArray{question};
    client->inspectionReady({},"codex/hgs/dashboard",stale);QVERIFY(card->isHidden());
    auto *list=window.findChild<QListWidget *>("sessionList");
    const auto key=list->currentItem()->data(Qt::UserRole).toString();
    card->answerRequested(key,"optional-one",QJsonArray{QJsonObject{{"question_id","q_0"},{"text","Do not resend"}}});
    QCOMPARE(messages.size(),0);QCOMPARE(promoted.size(),0);
    client->inspectionReady({},"codex/hgs/dashboard",details);
    if(!qEnvironmentVariable("HGS_PREVIEW_DIR").isEmpty()) {
        window.resize(1240,900);QTest::qWait(30);
        QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PREVIEW_DIR")+"/queued-question-answer.png"));
    }
    window.showSession({},"kimi/docs/research");window.showSession({},"codex/hgs/dashboard");
    QTRY_VERIFY(activity->findChild<QWidget *>("activityInputQueue")->isVisible());QVERIFY(card->isHidden());
    {
        SessionsWindow reopened(FakeCli::program(program.fileName()));reopened.setFleet(fleet());reopened.show();reopened.showSession({},"codex/hgs/dashboard");
        auto *reopenedActivity=reopened.findChild<ActivityView *>("mainActivity");
        QTRY_VERIFY(reopenedActivity->findChild<QWidget *>("activityInputQueue")->isVisible());
        QVERIFY(reopened.findChild<QuestionCard *>()->isHidden());
        // Recognition from a persisted receipt survives a later stale snapshot.
        reopened.findChild<HgsClient *>()->inspectionReady({},"codex/hgs/dashboard",stale);
        QVERIFY(reopened.findChild<QuestionCard *>()->isHidden());
    }
    auto nextQuestion=question;nextQuestion["question_id"]="optional-two";nextQuestion["question_hash"]="hash-two";
    nextQuestion["questions"]=QJsonArray{QJsonObject{{"id","q_0"},{"question","Another unanswered question?"},{"allow_other",true}}};
    const auto submittedQuestion=details["pending_questions"].toArray().first();
    details["pending_questions"]=QJsonArray{submittedQuestion,nextQuestion};QVERIFY(save());
    client->inspectionReady({},"codex/hgs/dashboard",details);
    // An inspection already in flight may still return the prior submitted-only
    // snapshot while the new question draft loads. Wait for the exact offered
    // form, not an empty card (which also has no draft left to load).
    QTRY_VERIFY(card->isVisible() && !card->isDraftLoading()
        && questionPrompts(card) == QStringList{"Another unanswered question?"}
        && card->findChild<QLineEdit *>("questionFreeText"));
    QVERIFY(card->isVisible());QCOMPARE(card->findChild<QLabel *>("questionPendingCount")->text(),QString("1 pending"));
    QCOMPARE(questionPrompts(card),QStringList{"Another unanswered question?"});
    QCOMPARE(card->findChild<QLabel *>("questionHeading")->text(),QString("Optional question"));
    QVERIFY(card->findChild<QToolButton *>("questionQueuePrevious")->isHidden());
    QVERIFY(card->findChild<QToolButton *>("questionQueueNext")->isHidden());
    card->findChild<QLineEdit *>("questionFreeText")->setText("Keep the remaining answer draft");
    client->inspectionReady({},"codex/hgs/dashboard",details);
    QCOMPARE(card->findChild<QLineEdit *>("questionFreeText")->text(),QString("Keep the remaining answer draft"));
    window.showSession({},"kimi/docs/research");window.showSession({},"codex/hgs/dashboard");
    QTRY_VERIFY(card->isVisible() && !card->isDraftLoading()
        && questionPrompts(card) == QStringList{"Another unanswered question?"}
        && card->findChild<QLineEdit *>("questionFreeText"));
    QCOMPARE(card->findChild<QLineEdit *>("questionFreeText")->text(),QString("Keep the remaining answer draft"));
    QFile calls(directory.filePath("calls"));QVERIFY(calls.open(QIODevice::ReadOnly));QCOMPARE(calls.readAll(),QByteArray("call\n"));
    details["pending_questions"]=QJsonArray{};details.remove("input_queue");QVERIFY(save());client->inspectionReady({},"codex/hgs/dashboard",details);
    QTRY_VERIFY(card->isHidden());QCOMPARE(composer->editor()->toPlainText(),QString("Keep my next-message draft"));
    QCOMPARE(promoted.size(),0);
}

void TestSessionsWindow::terminalChoiceOpensOnlyTheRequestingTerminal_data()
{
    QTest::addColumn<QString>("prefix");QTest::addColumn<QString>("source");QTest::addColumn<QString>("item");QTest::addColumn<QString>("option");
    QTest::addColumn<QString>("installer");QTest::addColumn<QString>("notice");
    QTest::newRow("hook review")<<"codex-hooks-trust:"<<"codex_hooks_trust"<<"hooks_trust"<<"review"<<QString()<<QString();
    // Hosts before the installer state keep the receipt without codex_update.
    QTest::newRow("codex update")<<"codex-update:"<<"codex_update"<<"codex_update"<<"update_now"<<QString()<<QString();
    QTest::newRow("codex update installing")<<"codex-update:"<<"codex_update"<<"codex_update"<<"update_now"<<"installing"
        <<"Codex in codex/hgs/dashboard (arch) is installing its update in Terminal. Resume the session after Codex exits.";
    QTest::newRow("codex update exited")<<"codex-update:"<<"codex_update"<<"codex_update"<<"update_now"<<"exited"
        <<"Codex in codex/hgs/dashboard (arch) exited after its update. Check the result in Terminal, then resume the session.";
    QTest::newRow("unknown installer state")<<"codex-update:"<<"codex_update"<<"codex_update"<<"update_now"<<"verifying"<<QString();
    QTest::newRow("installer state of another choice")<<"codex-hooks-trust:"<<"codex_hooks_trust"<<"hooks_trust"<<"review"<<"installing"<<QString();
}

void TestSessionsWindow::terminalChoiceOpensOnlyTheRequestingTerminal()
{
    QFETCH(QString,prefix);QFETCH(QString,source);QFETCH(QString,item);QFETCH(QString,option);QFETCH(QString,installer);QFETCH(QString,notice);
    QTemporaryDir directory;QFile file(directory.filePath("hgs"));QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QString(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
args=sys.argv[1:];root=pathlib.Path(__file__).parent
hash=('b' if (root/'second').exists() else 'a')*64
question={'question_id':'%1'+hash,'question_hash':hash,'run_id':'startup-run','conversation_id':None,
 'source':'%2','answer_transport':'codex_tui','can_answer':True,
 'questions':[{'id':'%3','question':'Startup choice','allow_other':False,
 'options':[{'id':'%4','label':'Continue in Terminal'},{'id':'other','label':'Continue here'}]}]}
if args[0]=='inspect':
 print(json.dumps({'tracked':True,'run_id':'startup-run','conversation_id':None,'runtime_state':'live','process_state':'running',
 'activity':'busy','phase':'approval','pending_questions':[question] if not (root/'done').exists() else [],'events':[],'cursor':0}))
elif args[0]=='answer':
 p=json.load(sys.stdin);time.sleep(.15);(root/'done').touch()
 receipt={'status':'answered','request_id':p['request_id'],'name':args[1],
 'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'question_id':p['question_id'],
 'question_hash':p['expected_question_hash'],'open_terminal':True}
 if '%5':receipt['codex_update']='%5'
 print(json.dumps(receipt))
)PY").arg(prefix,source,item,option,installer).toUtf8());file.close();QVERIFY(FakeCli::makeRunnable(file.fileName()));
    SessionsWindow window(FakeCli::program(file.fileName()));window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *card=window.findChild<QuestionCard *>();auto *client=window.findChild<HgsClient *>();
    auto *list=window.findChild<QListWidget *>("sessionList");auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");
    QSignalSpy answered(client,&HgsClient::questionAnswered);const auto key=list->currentItem()->data(Qt::UserRole).toString();
    // Answer only the exact question the card offers. An inspection that read the
    // fixture before the question changed can still deliver the older one, and the
    // window correctly refuses an answer to a question it does not offer.
    const auto offers=[&](const QString &question){return card->isVisible() && !card->isDraftLoading() && !card->submissionSnapshot(key,question).isEmpty();};
    const QJsonArray answer{QJsonObject{{"question_id",item},{"selected_option_ids",QJsonArray{option}},{"text",""}}};
    const QString id=prefix+QString(64,QChar('a')),secondId=prefix+QString(64,QChar('b'));
    QTRY_VERIFY(offers(id));
    auto *noticeLabel=window.findChild<QLabel *>("notice");QVERIFY(noticeLabel);
    card->answerRequested(key,id,answer);QTRY_COMPARE(answered.size(),1);
    QCOMPARE(tabs->currentWidget()->objectName(),QString("terminalView"));
    // Codex's installer state is explained beside Terminal; nothing else is announced.
    QCOMPARE(noticeLabel->text(),notice);QCOMPARE(noticeLabel->isVisible(),!notice.isEmpty());
    window.findChild<QPushButton *>("dismissNotice")->click();QVERIFY(noticeLabel->isHidden());
    // A delayed review acknowledgement must not steal another session's tab.
    QVERIFY(QFile::remove(directory.filePath("done")));QFile second(directory.filePath("second"));QVERIFY(second.open(QIODevice::WriteOnly));second.close();tabs->setCurrentIndex(0);
    window.showSession({},"kimi/docs/research");window.showSession({},"codex/hgs/dashboard");QTRY_VERIFY(offers(secondId));
    card->answerRequested(key,secondId,answer);window.showSession({},"kimi/docs/research");tabs->setCurrentIndex(0);
    QTRY_COMPARE(answered.size(),2);QVERIFY(tabs->currentWidget()->objectName()!="terminalView");
    // It still names the session whose Codex is updating.
    QCOMPARE(noticeLabel->text(),notice);
}

void TestSessionsWindow::optionalQuestionNotificationsAreOptIn()
{
    SessionsWindow window(program());
    auto *settings = window.findChild<QWidget *>("settingsPage"); QVERIFY(settings);
    auto *check = settings->findChild<QCheckBox *>("notifyOptionalQuestions"); QVERIFY(check);
    QVERIFY(!check->isChecked()); QVERIFY(!QuestionNotificationSettings::enabled());
    check->setChecked(true);
    QVERIFY(QuestionNotificationSettings::enabled());
    QCOMPARE(QSettings().value("notifications/optionalQuestions").toBool(), true);
    SessionsWindow reopened(program());
    QVERIFY(reopened.findChild<QCheckBox *>("notifyOptionalQuestions")->isChecked());
    check->setChecked(false); QVERIFY(!QuestionNotificationSettings::enabled());
}

void TestSessionsWindow::optionalQuestionKeepsComposerAvailable()
{
    QJsonObject question{{"question_id","optional-one"},{"question_hash","hash"},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"optional",true},{"can_answer",false},{"can_skip",true},{"questions",QJsonArray{QJsonObject{{"id","q_0"},{"question","Which option?"},{"required",false},{"allow_other",true},{"options",QJsonArray{}}}}}};
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},{"runtime_state","live"},{"process_state","running"},
        {"activity","idle"},{"phase","idle"},{"pending_questions",QJsonArray{}}};
    QFile original(script()); QVERIFY(original.open(QIODevice::ReadOnly));
    const auto fixture = m_dir->filePath("optional-question-hgs");
    QVERIFY(FakeCli::install(fixture, original.readAll().replace("if False:pass\n",
        "if False:pass\nelif c=='inspect':print(open(__file__+'.details').read())\n")));
    QVERIFY(FakeCli::replace(fixture + ".details", QJsonDocument(details).toJson()));
    SessionsWindow window(FakeCli::program(fixture));window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *card=window.findChild<QuestionCard *>();
    const auto applyDetails = [&] {
        if (!FakeCli::replace(fixture + ".details", QJsonDocument(details).toJson())) return false;
        // Wait for an inspection started after the atomic update: an older
        // in-flight reply must not replace the form while its draft loads.
        QSignalSpy replies(client, &HgsClient::inspectionReady); qsizetype issued = -1;
        return QTest::qWaitFor([&] {
            if (issued < 0) {
                if (!client->requestInspection({}, "codex/hgs/dashboard")) return false;
                issued = replies.size();
            }
            return std::any_of(replies.cbegin() + issued, replies.cend(), [&](const QList<QVariant> &reply) {
                return reply[0].toString().isEmpty() && reply[1].toString() == "codex/hgs/dashboard"
                    && reply[2].toJsonObject() == details && reply[3].toString().isEmpty();
            });
        });
    };
    QVERIFY(applyDetails());
    setComposerText(composer, "An unrelated follow-up");
    details["pending_questions"] = QJsonArray{question}; QVERIFY(applyDetails());
    QVERIFY(card->isVisible());QTRY_VERIFY(!card->isDraftLoading());QVERIFY(composer->isInputVisible());QVERIFY(composer->findChild<QPushButton *>("sendMessage")->isEnabled());
    QVERIFY(card->findChild<QPushButton *>("skipQuestion")->isEnabled());
    QVERIFY(applyDetails());QCOMPARE(composer->editor()->toPlainText(),QString("An unrelated follow-up"));
    auto latest=question;latest["question_id"]="latest";latest["created_at"]=20;
    latest["questions"]=QJsonArray{QJsonObject{{"id","q_0"},{"question","Most recent question?"},{"required",false},{"allow_other",true},{"options",QJsonArray{}}}};
    question["created_at"]=10;question["can_answer"]=true;
    auto olderItems=question["questions"].toArray();auto secondItem=olderItems.first().toObject();secondItem["id"]="q_1";secondItem["question"]="Another older question?";olderItems.append(secondItem);question["questions"]=olderItems;
    details["pending_questions"]=QJsonArray{question,latest,question,QJsonObject{}};QVERIFY(applyDetails());
    QCOMPARE(card->findChild<QLabel *>("questionPendingCount")->text(),QString("3 pending"));
    QStringList labels=questionPrompts(card);
    QVERIFY(labels.contains("Most recent question?"));QVERIFY(!labels.contains("Which option?"));
    auto *previous=card->findChild<QToolButton *>("questionQueuePrevious"),*next=card->findChild<QToolButton *>("questionQueueNext");
    QVERIFY(previous->isVisible());QVERIFY(!previous->isEnabled());QVERIFY(next->isEnabled());
    QCOMPARE(card->findChild<QLabel *>("questionHeading")->text(),QString("Optional question #1/2"));
    QSignalSpy answers(card,&QuestionCard::answerRequested);
    const auto firstAnswer=[card] {
        for(auto *editor:card->findChildren<QLineEdit *>("questionFreeText"))
            if(editor->property("questionId").toString()=="q_0")return editor;
        return static_cast<QLineEdit *>(nullptr);
    };
    const auto answerReady = [&](const QString &prompt) {
        return card->isVisible() && !card->isDraftLoading() && questionPrompts(card).contains(prompt) && firstAnswer();
    };
    QTRY_VERIFY(answerReady("Most recent question?"));firstAnswer()->setText("Latest draft");
    next->click();QTRY_VERIFY(!card->isDraftLoading());
    labels=questionPrompts(card);
    QVERIFY(labels.contains("Which option?"));QVERIFY(!labels.contains("Most recent question?"));
    QVERIFY(previous->isEnabled());QVERIFY(!next->isEnabled());
    QCOMPARE(card->findChild<QLabel *>("questionHeading")->text(),QString("Optional question #2/2"));
    QTRY_VERIFY(answerReady("Which option?"));firstAnswer()->setText("Older draft");
    auto *pages=card->findChild<QTabBar *>("questionTabs");pages->setCurrentIndex(1);
    previous->click();QTRY_VERIFY(!card->isDraftLoading());
    QTRY_VERIFY(answerReady("Most recent question?") && firstAnswer()->text() == "Latest draft");
    next->click();QTRY_VERIFY(!card->isDraftLoading());
    QTRY_VERIFY(answerReady("Which option?") && firstAnswer()->text() == "Older draft");
    QCOMPARE(card->findChild<QTabBar *>("questionTabs")->currentIndex(),1);
    auto newest=latest;newest["question_id"]="newest";newest["created_at"]=30;
    details["pending_questions"]=QJsonArray{newest,question,latest};QVERIFY(applyDetails());
    QCOMPARE(card->findChild<QLabel *>("questionPendingCount")->text(),QString("4 pending"));
    QCOMPARE(card->findChild<QLabel *>("questionHeading")->text(),QString("Optional question #3/3"));
    QTRY_VERIFY(answerReady("Which option?") && firstAnswer()->text() == "Older draft");
    // A required confirmation preempts browsing, then returns to the chosen draft.
    auto required=latest;required["optional"]=false;required["question_id"]="required";required["can_answer"]=true;
    details["pending_questions"]=QJsonArray{newest,question,required,latest};QVERIFY(applyDetails());
    QVERIFY(previous->isHidden());QVERIFY(next->isHidden());QVERIFY(!composer->isInputVisible());
    QCOMPARE(card->findChild<QLabel *>("questionHeading")->text(),QString("Agent needs your answer"));
    card->queueNavigationRequested(1); // Stale navigation cannot bypass a required request.
    QVERIFY(!composer->isInputVisible());QVERIFY(next->isHidden());
    details["pending_questions"]=QJsonArray{newest,question,latest};QVERIFY(applyDetails());
    QTRY_VERIFY(answerReady("Which option?") && firstAnswer()->text() == "Older draft");
    QVERIFY(composer->isInputVisible());QCOMPARE(answers.size(),0);
    // A different conversation must start from its own newest question.
    details["conversation_id"]="conversation-two";QVERIFY(applyDetails());
    QVERIFY(applyDetails()); // Initial history fetch after the conversation changed.
    QTRY_VERIFY(answerReady("Most recent question?"));
    QCOMPARE(firstAnswer()->text(),QString());
    QVERIFY(!previous->isEnabled());
    details["conversation_id"]="conversation-one";QVERIFY(applyDetails());
    QVERIFY(applyDetails());
    QTRY_VERIFY(answerReady("Which option?") && firstAnswer()->text() == "Older draft");
    // When the selected request resolves elsewhere, fall back to a pending one.
    details["pending_questions"]=QJsonArray{latest};QVERIFY(applyDetails());
    QTRY_VERIFY(answerReady("Most recent question?") && firstAnswer()->text() == "Latest draft");
    QVERIFY(previous->isHidden());QVERIFY(next->isHidden());
    details["pending_questions"]=QJsonArray{};QVERIFY(applyDetails());
    QVERIFY(card->isHidden());QVERIFY(composer->isInputVisible());QCOMPARE(composer->editor()->toPlainText(),QString("An unrelated follow-up"));
}

void TestSessionsWindow::questionCompletionKeepsInputFocus_data()
{
    QTest::addColumn<QString>("action");
    for (const auto &action : {"mouse", "keyboard", "navigate", "external"}) QTest::newRow(action) << QString(action);
}

void TestSessionsWindow::questionCompletionKeepsInputFocus()
{
    QFETCH(QString, action);
    QTemporaryDir directory;
    QFile program(directory.filePath("hgs")); QVERIFY(program.open(QIODevice::WriteOnly));
    program.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
if sys.argv[1]=='inspect':
 data=json.loads((root/'details.json').read_text())
 if (root/'done').exists(): data['pending_questions']=[]
 print(json.dumps(data))
elif sys.argv[1]=='answer':
 p=json.load(sys.stdin)
 (root/'submitted').touch()
 for _ in range(1000):
  if (root/'complete').exists(): break
  time.sleep(.01)
 (root/'done').touch()
 print(json.dumps({'status':'answered','request_id':p['request_id'],'name':sys.argv[2],
  'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
  'question_id':p['question_id'],'question_hash':p['expected_question_hash']}))
)PY");
    program.close(); QVERIFY(FakeCli::makeRunnable(program.fileName()));
    const QJsonObject question{{"question_id","question-one"},{"question_hash","hash-one"},{"run_id","run-one"},
        {"conversation_id","conversation-one"},{"can_answer",true},{"questions",QJsonArray{QJsonObject{
            {"id","q_0"},{"header","Scope"},{"question","Which area?"},{"allow_other",true},
            {"options",QJsonArray{QJsonObject{{"id","opt_0_0"},{"label","Documents"}}}}}}}};
    QJsonObject details{{"tracked",true},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"runtime_state","live"},{"process_state","running"},{"pending_questions",QJsonArray{question}},
        {"session_usage",QJsonObject{{"context",QJsonObject{{"used",236000}}}}},{"events",QJsonArray{}},{"cursor",0}};
    QFile fixture(directory.filePath("details.json")); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(QJsonDocument(details).toJson()); fixture.close();
    SessionsWindow window(FakeCli::program(program.fileName())); window.setFleet(fleet()); window.show(); window.activateWindow();
    window.showSession({},"kimi/docs/research");
    auto *card=window.findChild<QuestionCard *>(); auto *composer=window.findChild<MessageComposer *>("messageComposer");
    auto *client=window.findChild<HgsClient *>(); auto *context=window.findChild<QPushButton *>("activityContext");
    auto *submit=card->findChild<QPushButton *>("submitQuestionAnswer");
    QSignalSpy answered(client,&HgsClient::questionAnswered);
    QTRY_VERIFY(card->isVisible()); QTRY_VERIFY(!card->isDraftLoading()); QTRY_VERIFY(window.isActiveWindow());
    auto *option=card->findChild<QAbstractButton *>("questionOption"); QVERIFY(option);
    option->click(); option->setFocus(Qt::TabFocusReason); QTRY_VERIFY(option->hasFocus());
    setComposerText(composer, "Keep this draft");
    composer->editor()->moveCursor(QTextCursor::End);
    if(action=="external") {
        details["pending_questions"]=QJsonArray{};
        client->inspectionReady({},"kimi/docs/research",details);
    } else {
        submit->setFocus(Qt::TabFocusReason);
        if(action=="keyboard") QTest::keyClick(submit,Qt::Key_Space);
        else QTest::mouseClick(submit,Qt::LeftButton);
        QVERIFY(!context->hasFocus());
        QTRY_VERIFY(QFile::exists(directory.filePath("submitted")));
        if(action=="navigate") {context->setFocus(Qt::TabFocusReason); QVERIFY(context->hasFocus());}
        QFile complete(directory.filePath("complete")); QVERIFY(complete.open(QIODevice::WriteOnly)); complete.close();
        QTRY_COMPARE(answered.size(),1);
    }
    QTRY_VERIFY(card->isHidden()); QVERIFY(composer->isInputVisible());
    if(action=="navigate") QVERIFY(context->hasFocus());
    else {QVERIFY(composer->editor()->hasFocus()); QTest::keyClicks(composer->editor()," + next");}
    QCOMPARE(composer->editor()->toPlainText(),action=="navigate" ? QString("Keep this draft") : QString("Keep this draft + next"));
    // Ordinary polling must preserve the chosen target after the card disappears.
    details["pending_questions"]=QJsonArray{}; client->inspectionReady({},"kimi/docs/research",details);
    QVERIFY(action=="navigate" ? context->hasFocus() : composer->editor()->hasFocus());
}

void TestSessionsWindow::questionFooterHasRoomAndKeepsAnswers_data()
{
    QTest::addColumn<QString>("theme");QTest::addColumn<QSize>("size");
    for(const auto *theme:{"dark","light"})for(const auto size:{QSize(1100,780),QSize(960,600)})
        QTest::newRow(qPrintable(QString("%1-%2").arg(theme).arg(size.height())))<<QString(theme)<<size;
}

void TestSessionsWindow::questionFooterHasRoomAndKeepsAnswers()
{
    QFETCH(QString,theme);QFETCH(QSize,size);QSettings().setValue("workspace/theme",theme);
    auto state=fleet();auto box=state.local();auto &session=box.sessions[0];
    session.conversationId="conversation-one";session.runId="run-one";session.phase="input";session.activity="waiting";session.attentionId="question-one";
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.resize(size);window.setFleet(state);
    // The window's own inspections answer with the base fixture, which reports no
    // pending question in the same conversation. One arriving after the injected
    // details replaces them and hides the card, or clears it before the answers are
    // chosen. Inspect only on show and selection, then wait for an inspection of the
    // test's own: HgsClient runs one per session, so once its reply is in, none is
    // pending and no poll follows.
    window.setDetailsPollForTests(500,24*60*60*1000);window.show();
    auto *client=window.findChild<HgsClient *>();QSignalSpy inspections(client,&HgsClient::inspectionReady);
    window.showSession({},session.name);
    qsizetype issued=-1;
    // Under load the window's inspection may run until HgsClient's 5 s limit.
    QTRY_VERIFY_WITH_TIMEOUT(issued>=0 || (client->requestInspection({},session.name) && (issued=inspections.size(),true)),15000);
    QTRY_VERIFY_WITH_TIMEOUT(std::any_of(inspections.cbegin()+issued,inspections.cend(),[&](const QList<QVariant> &reply){
        return reply[0].toString().isEmpty() && reply[1].toString()==session.name && reply[3].toString().isEmpty();}),15000);
    QJsonArray questions;
    for(int i=0;i<3;++i)questions<<QJsonObject{{"id",QString("q_%1").arg(i)},{"header",QString("Choice %1").arg(i+1)},
        {"question","Choose the intended behavior when the requested font weight is unavailable and the text uses the regular face."},{"allow_other",true},
        {"options",QJsonArray{QJsonObject{{"id",QString("opt_%1_0").arg(i)},{"label","Show a warning (Recommended)"},{"description","Report the missing font weight once for each family and weight, while keeping the current font files."}},
            QJsonObject{{"id",QString("opt_%1_1").arg(i)},{"label","Include another font face"},{"description","Ship an additional font file with the application."}}}}};
    QJsonObject question{{"question_id","question-one"},{"question_hash","hash-one"},{"run_id","run-one"},{"conversation_id","conversation-one"},
        {"can_answer",false},{"answer_unavailable_reason","The visible Claude question differs from this request"},{"questions",questions}};
    QJsonObject details{{"tracked",true},{"conversation_id","conversation-one"},{"run_id","run-one"},{"process_state","running"},{"runtime_state","live"},
        {"activity","waiting"},{"phase","input"},{"pending_questions",QJsonArray{question}},{"session_usage",QJsonObject{{"context",QJsonObject{{"used",236000}}}}}};
    client->inspectionReady({},session.name,details);
    auto *card=window.findChild<QuestionCard *>();QTRY_VERIFY(!card->isDraftLoading());auto *tabs=card->findChild<QTabBar *>("questionTabs");
    for(int i=0;i<3;++i){tabs->setCurrentIndex(i);for(auto *option:card->findChildren<QAbstractButton *>("questionOption"))if(option->property("optionId")==QString("opt_%1_0").arg(i))option->click();}
    auto *submit=card->findChild<QPushButton *>("submitQuestionAnswer");QVERIFY(!submit->isEnabled());
    // A refreshed capability unlocks submission without rebuilding the user's answers.
    question["can_answer"]=true;question["answer_unavailable_reason"]="";details["pending_questions"]=QJsonArray{question};
    client->inspectionReady({},session.name,details);QVERIFY(submit->isEnabled());QCOMPARE(tabs->currentIndex(),2);
    QCOMPARE(card->findChild<QLabel *>("questionProgress")->text(),QString("3 of 3 answered"));
    auto *read=window.findChild<QPushButton *>("activityMarkRead");auto *context=window.findChild<QPushButton *>("activityContext");
    QTest::qWait(30);QVERIFY(card->isVisible());QVERIFY(read->isVisible());
    const auto bounds=[&](QWidget *widget){return QRect(widget->mapTo(&window,QPoint()),widget->size());};
    QVERIFY(bounds(read).top()-bounds(card).bottom()>=8);QVERIFY(bounds(context).top()-bounds(card).bottom()>=8);
    QVERIFY(bounds(read).right()<bounds(context).left());QVERIFY(bounds(read).bottom()<window.height());
    const auto preview=qEnvironmentVariable("HGS_QUESTION_FOOTER_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+QString("/question-footer-%1-%2.png").arg(theme).arg(size.height())));}
}

void TestSessionsWindow::startupTrustAnswerBeforeConversation_data()
{
    QTest::addColumn<bool>("nullConversation"); QTest::addColumn<QString>("choice"); QTest::addColumn<QString>("provider"); QTest::addColumn<bool>("permissionMode");
    QTest::newRow("null-trust") << true << QString("trust_0") << QString("kimi") << false;
    QTest::newRow("codex-null-trust") << true << QString("trust_0") << QString("codex") << false;
    QTest::newRow("codex-empty-decline") << false << QString("trust_1") << QString("codex") << false;
    QTest::newRow("claude-null-trust") << true << QString("trust_0") << QString("claude") << false;
    QTest::newRow("null-decline") << true << QString("trust_1") << QString("kimi") << false;
    QTest::newRow("claude-null-decline") << true << QString("trust_1") << QString("claude") << false;
    QTest::newRow("empty-trust") << false << QString("trust_0") << QString("kimi") << false;
    QTest::newRow("claude-empty-trust") << false << QString("trust_0") << QString("claude") << false;
    QTest::newRow("empty-decline") << false << QString("trust_1") << QString("kimi") << false;
    QTest::newRow("claude-empty-decline") << false << QString("trust_1") << QString("claude") << false;
    QTest::newRow("claude-permission-auto") << true << QString("mode_0") << QString("claude") << true;
    QTest::newRow("claude-permission-keep") << false << QString("mode_1") << QString("claude") << true;
}

void TestSessionsWindow::startupTrustAnswerBeforeConversation()
{
    QFETCH(bool, nullConversation); QFETCH(QString, choice); QFETCH(QString, provider); QFETCH(bool, permissionMode);
    QTemporaryDir directory;
    const QString hash(64, QChar('a')), id = (permissionMode ? "claude-permissions:" : provider + "-trust:") + hash;
    const QString item = permissionMode ? "permission_mode" : "trust";
    const QJsonValue conversation = nullConversation ? QJsonValue(QJsonValue::Null) : QJsonValue(permissionMode ? "started-conversation" : "");
    const QJsonObject question{{"question_id", id}, {"question_hash", hash}, {"run_id", "startup-run"},
        {"conversation_id", conversation}, {"can_answer", true}, {"source", permissionMode ? "claude_permission_mode" : provider + "_folder_trust"},
        {"answer_transport", provider + "_tui"}, {"questions", QJsonArray{QJsonObject{{"id", item},
            {"question", permissionMode ? "Make auto mode your default permission mode?" : "Trust this folder?"}, {"body", "/work/example\nProject MCP targets:\ntracker (stdio): command=example"},
            {"allow_other", false}, {"options", QJsonArray{
                QJsonObject{{"id", permissionMode ? "mode_0" : "trust_0"}, {"label", permissionMode ? "Yes, set auto mode as my default permission mode" : "Trust this folder"}},
                QJsonObject{{"id", permissionMode ? "mode_1" : "trust_1"}, {"label", permissionMode ? "No, keep bypass permissions" : "Don't trust"}}}}}}}};
    QFile snapshot(directory.filePath("inspect.json")); QVERIFY(snapshot.open(QIODevice::WriteOnly));
    snapshot.write(QJsonDocument(QJsonObject{{"tracked", true}, {"run_id", "startup-run"},
        {"conversation_id", conversation}, {"runtime_state", "live"}, {"process_state", "running"},
        {"activity", "busy"}, {"phase", "approval"}, {"pending_questions", QJsonArray{question}},
        {"events", QJsonArray{}}, {"cursor", 0}}).toJson()); snapshot.close();
    QFile program(directory.filePath("hgs")); QVERIFY(program.open(QIODevice::WriteOnly));
    program.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(__file__).parent
if sys.argv[1]=='inspect':
 data=json.loads((root/'inspect.json').read_text())
 if (root/'answer.json').exists(): data['pending_questions']=[]
 print(json.dumps(data))
elif sys.argv[1]=='answer':
 p=json.load(sys.stdin);(root/'answer.json').write_text(json.dumps(p))
 with (root/'calls').open('a') as f:f.write('call\n')
 print(json.dumps({'status':'answered','request_id':p['request_id'],'name':sys.argv[2],
  'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],
  'question_id':p['question_id'],'question_hash':p['expected_question_hash']}))
)PY"); program.close();
    QVERIFY(FakeCli::makeRunnable(program.fileName()));
    SessionsWindow window(FakeCli::program(program.fileName())); window.setFleet(fleet()); window.show(); window.showSession({}, "kimi/docs/research");
    auto *card = window.findChild<QuestionCard *>(); auto *client = window.findChild<HgsClient *>();
    QSignalSpy answered(client, &HgsClient::questionAnswered), failed(client, &HgsClient::questionAnswerFailed);
    QTRY_VERIFY(card->isVisible()); QTRY_VERIFY(!card->isDraftLoading());
    auto *submit = card->findChild<QPushButton *>("submitQuestionAnswer");
    QVERIFY(!submit->isEnabled()); // Native highlighted option is not consent.
    QAbstractButton *selected = nullptr;
    for (auto *button : card->findChildren<QAbstractButton *>("questionOption")) {
        QVERIFY(!button->isChecked());
        if (button->property("optionId").toString() == choice) selected = button;
    }
    QVERIFY(selected); selected->click(); QVERIFY(submit->isEnabled()); submit->click();
    QTRY_COMPARE(answered.size(), 1); QCOMPARE(failed.size(), 0);
    QFile sent(directory.filePath("answer.json")); QVERIFY(sent.open(QIODevice::ReadOnly));
    const auto payload = QJsonDocument::fromJson(sent.readAll()).object();
    QCOMPARE(payload["expected_run_id"].toString(), "startup-run");
    QVERIFY(payload["expected_conversation_id"].isString());
    QCOMPARE(payload["expected_conversation_id"].toString(), conversation.toString());
    QCOMPARE(payload["question_id"].toString(), id); QCOMPARE(payload["expected_question_hash"].toString(), hash);
    const QJsonArray expected{QJsonObject{{"question_id", item},
        {"selected_option_ids", QJsonArray{choice}}, {"text", ""}}};
    QCOMPARE(payload["answers"].toArray(), expected);
    QTRY_VERIFY(!card->isVisible());
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestSessionsWindow::activityEscapingAndActions()
{
    QJsonObject data{{"tracked", true}, {"conversation_id", "conversation-one"}, {"phase", "idle"}, {"activity", "idle"},
        {"cwd", "/workspace/zerus"}, {"prompt", "<b>literal request</b>"}, {"cursor", 5},
        {"events", QJsonArray{QJsonObject{{"seq", 4}, {"at", 1790928000}, {"type", "PostToolUse"}, {"tool", "Bash"}, {"detail", "<img src='file:///not-loaded'>"}},
            QJsonObject{{"seq", 5}, {"at", 1790928001}, {"type", "Stop"}, {"detail", QString("A response paragraph.\n\n").repeated(60)}}}}};
    const auto fixture = programInspecting("activity-actions-hgs", data); QVERIFY(!fixture.isEmpty());
    SessionsWindow window(fixture); window.setFleet(fleet()); window.setClipboardMode(true); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList"); list->setCurrentRow(0);
    auto *client = window.findChild<HgsClient *>(); QVERIFY(client);
    client->inspectionReady({}, "codex/hgs/dashboard", data);
    auto *activity = window.findChild<QTextBrowser *>("activity");
    QTRY_VERIFY(activity->toPlainText().contains("<b>literal request</b>"));
    QTRY_VERIFY(activity->toPlainText().contains("<img src='file:///not-loaded'>"));
    QVERIFY(window.findChild<QAction *>("pauseAction")->isEnabled());
    QSignalSpy opened(&window, &SessionsWindow::sessionActivated);
    QSignalSpy copied(&window, &SessionsWindow::copySessionCommandRequested);
    auto *view = window.findChild<ActivityView *>("mainActivity");
    auto *scroll = activity->verticalScrollBar(); QTRY_VERIFY(scroll->maximum() > 0);
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Keep my draft");
    scroll->setValue(0);
    auto cursor = activity->textCursor(); cursor.movePosition(QTextCursor::Start); cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 5);
    activity->setTextCursor(cursor);
    const auto point = list->visualItemRect(list->currentItem()).center();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, point);
    QCOMPARE(scroll->value(), 0); QVERIFY(activity->textCursor().hasSelection());
    QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, point);
    QTRY_COMPARE(scroll->value(), scroll->maximum()); QVERIFY(!activity->textCursor().hasSelection()); QVERIFY(view->jumpButton()->isHidden());
    QCOMPARE(opened.size(), 0); QCOMPARE(copied.size(), 0); QCOMPARE(composer->editor()->toPlainText(), QString("Keep my draft"));
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); tabs->setCurrentIndex(1);
    scroll->setValue(0); QTest::keyClick(list, Qt::Key_Return);
    QCOMPARE(tabs->currentIndex(), 0); QTRY_COMPARE(scroll->value(), scroll->maximum());
    QCOMPARE(opened.size(), 0); QCOMPARE(copied.size(), 0); QCOMPARE(composer->editor()->toPlainText(), QString("Keep my draft"));
    window.findChild<QAction *>("openSessionAction")->trigger(); QCOMPARE(opened.size(), 1);
    QCOMPARE(opened[0][1].toString(), QStringLiteral("codex/hgs/dashboard"));
    QSignalSpy completed(client, &HgsClient::writeDone);
    auto *pause = window.findChild<QAction *>("pauseAction"); pause->trigger();
    QVERIFY(!pause->isEnabled()); QTRY_COMPARE(completed.size(), 1);
    QVERIFY(completed[0][1].toBool());
}

void TestSessionsWindow::sessionContextMenuSignalsAndKeyboard()
{
    // A POSIX folder, also when this GUI runs on Windows.
    auto state = fleet(); auto box = state.local(); box.sessions[0].cwd = "/work/a folder"; box.metrics["platform"] = "linux";
    box.sessions[0].attached = 1; box.sessions[0].clients = {"/dev/pts/42"};
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.setClipboardMode(true); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList"); auto *menu = window.findChild<QMenu *>("sessionContextMenu"); QVERIFY(menu);
    QSignalSpy terminal(&window, &SessionsWindow::terminalOpenRequested), normal(&window, &SessionsWindow::sessionActivated), shell(&window, &SessionsWindow::folderShellRequested);
    QSignalSpy commands(&window, &SessionsWindow::copySessionCommandRequested), texts(&window, &SessionsWindow::copyTextRequested);
    const auto showMouseMenu = [&](int row) {
        const QPoint point = list->visualItemRect(list->item(row)).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, list->viewport()->mapToGlobal(point));
        QApplication::sendEvent(list->viewport(), &event);
    };
    list->setCurrentRow(1);
    const auto targetPoint = list->visualItemRect(list->item(0)).center();
    QTest::mousePress(list->viewport(), Qt::RightButton, {}, targetPoint);
    QTest::mouseRelease(list->viewport(), Qt::RightButton, {}, targetPoint);
    QCOMPARE(list->currentRow(), 1);
    showMouseMenu(0); QVERIFY(menu->isVisible()); QCOMPARE(list->currentRow(), 1);
    auto action = [&](const char *name) { return menu->findChild<QAction *>(name); };
    QVERIFY(!action("contextChangeSession")->isEnabled()); // Busy stays busy.
    action("contextCopyName")->trigger(); QCOMPARE(texts.size(), 1); QCOMPARE(texts[0][0].toString(), QString("codex/hgs/dashboard"));
    action("contextCopyFolder")->trigger(); QCOMPARE(texts.size(), 2); QCOMPARE(texts[1][0].toString(), QString("/work/a folder"));
    action("contextFolderShell")->trigger(); QCOMPARE(shell.size(),1); QCOMPARE(shell.last()[1].toString(),QString("/work/a folder"));
    QCOMPARE(list->currentRow(), 1); // Passive actions leave the conversation and draft in place.
    action("contextCopyCommand")->trigger(); QCOMPARE(commands.size(), 1); QCOMPARE(commands[0][0].toString(), QString());
    QCOMPARE(commands[0][1].toString(), QString("codex/hgs/dashboard")); QVERIFY(commands[0][2].toString().isEmpty());
    action("contextOpenTerminal")->trigger(); QCOMPARE(terminal.size(), 1); QCOMPARE(normal.size(), 0);
    QCOMPARE(terminal[0][2].toString(), QString("/dev/pts/42")); // Explicit open ignores clipboard default.
    menu->hide(); list->setFocus(); QTest::keyClick(list, Qt::Key_F10, Qt::ShiftModifier); QVERIFY(menu->isVisible());
    menu->hide(); QTest::keyClick(list, Qt::Key_Menu); QVERIFY(menu->isVisible()); menu->hide();
    const QPoint emptyPoint(5, list->viewport()->height() - 1);
    QContextMenuEvent empty(QContextMenuEvent::Mouse, emptyPoint, list->viewport()->mapToGlobal(emptyPoint));
    QApplication::sendEvent(list->viewport(), &empty); QVERIFY(!menu->isVisible());
    showMouseMenu(4); action("contextOpenTerminal")->trigger(); QCOMPARE(terminal.size(), 2);
    QCOMPARE(terminal[1][0].toString(), QString("mac")); QVERIFY(terminal[1][2].toString().isEmpty()); menu->hide();
    BoxState offline; offline.host = "mac"; offline.ok = false; state.setPeer(offline, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    showMouseMenu(4); QVERIFY(!action("contextOpenTerminal")->isEnabled()); QVERIFY(!action("contextRenameSession")->isEnabled());
    QVERIFY(!action("contextTerminateSession")->isEnabled()); QVERIFY(action("contextCopyCommand")->isEnabled());
    action("contextCopyCommand")->trigger(); QCOMPARE(commands.size(), 2); QCOMPARE(commands[1][0].toString(), QString("mac"));
    QVERIFY(!action("contextCopyFolder")->isEnabled()); // Missing path is not guessed from a project label.
}

void TestSessionsWindow::copySessionReferencePreservesSelectionAndDraft()
{
    QTemporaryDir fixture;QVERIFY(fixture.isValid());const auto program=fixture.filePath("hgs");
    QFile file(program);QVERIFY(file.open(QIODevice::WriteOnly));
    const auto uri=QStringLiteral("zerus://session?machine=11111111-1111-4111-8111-111111111111&host=fixture&name=codex%2Fhgs%2Fdashboard&conversation=conversation-one&run=run");
    file.write("#!/usr/bin/env python3\nimport runpy,sys\nif sys.argv[1:2]==['reference']:\n"
        " open(__file__+'.args','w').write(''.join(v+'\\n' for v in sys.argv[1:]));sys.stdout.write(open(__file__+'.reference').read())\n"
        "else:\n sys.argv[0]="+FakeCli::pythonString(script())+";runpy.run_path(sys.argv[0],run_name='__main__')\n");file.close();
    QVERIFY(FakeCli::makeRunnable(file.fileName()));
    QFile response(program+".reference");QVERIFY(response.open(QIODevice::WriteOnly));response.write(QJsonDocument(QJsonObject{{"uri",uri}}).toJson());response.close();
    auto state=fleet();auto box=state.local();box.sessions[0].runId="run";box.sessions[0].conversationId="conversation-one";state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(program));window.setFleet(state);window.show();window.showSession({},box.sessions[0].name);
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer, "Keep this draft");
    QSignalSpy copied(&window,&SessionsWindow::copyTextRequested),activated(&window,&SessionsWindow::sessionActivated);
    QVERIFY(!window.findChild<QPushButton *>("copySessionReferenceAction"));
    auto *list=window.findChild<QListWidget *>("sessionList");
    auto *menu=window.findChild<QMenu *>("sessionContextMenu");QVERIFY(menu);
    const auto referenceAction=[&]() {
        const QPoint point=list->visualItemRect(list->currentItem()).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse,point,list->viewport()->mapToGlobal(point));
        QApplication::sendEvent(list->viewport(),&event);
        return menu->findChild<QAction *>("contextCopyReference");
    };
    auto *action=referenceAction();QVERIFY(action);QVERIFY(action->isEnabled());action->trigger();menu->hide();
    QTRY_COMPARE(copied.size(),1);QCOMPARE(copied[0][0].toString(),uri);QCOMPARE(activated.size(),0);QCOMPARE(composer->editor()->toPlainText(),QString("Keep this draft"));
    QFile args(program+".args");QVERIFY(args.open(QIODevice::ReadOnly));QCOMPARE(args.readAll(),QByteArray("reference\ncodex/hgs/dashboard\n--json\n--run\nrun\n--conversation\nconversation-one\n"));
    box.ok=false;state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    action=referenceAction();QVERIFY(action);QVERIFY(!action->isEnabled());menu->hide();
}
void TestSessionsWindow::sessionIdentifiersRemainInDetailsAcrossNavigation()
{
    QSettings().setValue("workspace/inspectorVisible", true); QSettings().setValue("workspace/inspectorTab", 1);
    auto state = fleet(); auto box = state.local();
    box.ok = false; box.sessions[0].conversationId = "12345678-9abc-4def-8123-456789abcdef";
    box.sessions[1].conversationId.clear(); box.sessions[1].runId = "unconfirmed-run";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.resize(820, 700); window.setFleet(state); window.show();
    window.showSession({}, box.sessions[0].name);
    QVERIFY(!window.findChild<QLabel *>("sessionId"));
    auto *value = window.findChild<QLabel *>("detailsIdentityValue"); QVERIFY(value);
    auto *title = window.findChild<QLabel *>("detailsIdentityTitle"); QVERIFY(title);
    auto *copy = window.findChild<QPushButton *>("copySessionIdAction"); QVERIFY(copy); QTRY_VERIFY(copy->isVisible());
    QVERIFY(window.findChild<QTabWidget *>("sessionInspector")->isAncestorOf(value));
    const auto copyFits = [&] {
        for (auto *parent = copy->parentWidget(); parent; parent = parent->parentWidget())
            if (!parent->rect().contains(QRect(copy->mapTo(parent, QPoint()), copy->size()))) return false;
        return true;
    };
    QTRY_VERIFY(copyFits());
    window.resize(1280, 700);
    window.findChild<QSplitter *>("sessionWorkSplitter")->setSizes({600, 420});
    QTRY_VERIFY(value->width() >= value->fontMetrics().horizontalAdvance(value->text()));
    QTRY_VERIFY(copyFits());
    QCOMPARE(title->text(), QString("Session ID")); QCOMPARE(value->text(), box.sessions[0].conversationId);
    QVERIFY(value->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    QCOMPARE(copy->accessibleName(), QString("Copy session ID"));
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Keep this draft");
    QSignalSpy copied(&window, &SessionsWindow::copyTextRequested);
    copy->click(); QCOMPARE(copied.size(), 1); QCOMPARE(copied[0][0].toString(), box.sessions[0].conversationId);
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep this draft"));
    // The header icon copies the same identity without showing it outside Details.
    auto *headerCopy = window.findChild<QPushButton *>("headerCopySessionIdAction"); QVERIFY(headerCopy); QVERIFY(headerCopy->isVisible());
    QVERIFY(!window.findChild<QTabWidget *>("sessionInspector")->isAncestorOf(headerCopy));
    QCOMPARE(headerCopy->accessibleName(), QString("Copy session ID"));
    headerCopy->click(); QCOMPARE(copied.size(), 2); QCOMPARE(copied[1][0].toString(), box.sessions[0].conversationId);
    const auto preview = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (!preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(window.grab().save(preview + "/copy-session-id.png")); }
    window.showSession({}, box.sessions[1].name);
    QCOMPARE(title->text(), QString("Run ID")); QCOMPARE(value->text(), QString("unconfirmed-run"));
    QCOMPARE(copy->accessibleName(), QString("Copy run ID")); QCOMPARE(headerCopy->accessibleName(), QString("Copy run ID"));
    window.showSession({}, box.sessions[0].name); QCOMPARE(value->text(), box.sessions[0].conversationId);
    QCOMPARE(composer->editor()->toPlainText(), QString("Keep this draft"));
    window.findChild<HgsClient *>()->inspectionReady({}, box.sessions[0].name,
        {{"tracked", true}, {"conversation_id", "new-conversation"}, {"run_id", "new-run"}});
    QCOMPARE(value->text(), QString("new-conversation"));
    headerCopy->click(); QCOMPARE(copied.size(), 3); QCOMPARE(copied[2][0].toString(), QString("new-conversation"));
    box.sessions[1].runId.clear(); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    window.showSession({}, box.sessions[1].name); QCOMPARE(value->text(), QString("Not confirmed")); QVERIFY(!copy->isEnabled());
    QVERIFY(!headerCopy->isEnabled()); copy->click(); headerCopy->click(); QCOMPARE(copied.size(), 3);
}

void TestSessionsWindow::activityMarkReadKeepsReadingPosition_data()
{
    QTest::addColumn<QString>("theme");QTest::newRow("dark")<<QString("dark");QTest::newRow("light")<<QString("light");
}
void TestSessionsWindow::activityMarkReadKeepsReadingPosition()
{
    QFETCH(QString,theme);QSettings().setValue("workspace/theme",theme);
    auto state=fleet();auto box=state.local();auto &session=box.sessions[0];session.conversationId="conversation-one";
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());QVERIFY(state.setReviewLater({},session,true));
    QSettings().setValue("attention/sessionMarks",QJsonDocument(state.attentionMarks()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program());window.resize(1000,760);window.setFleet(state);window.show();
    auto *client=window.findChild<HgsClient *>();QSignalSpy inspections(client,&HgsClient::inspectionReady);
    window.showSession({},session.name);QTRY_VERIFY(!inspections.isEmpty());
    QJsonArray events;for(int i=1;i<=30;++i)events<<QJsonObject{{"seq",i},{"at",1791018000+i},{"type","AgentMessage"},{"detail",QString("Read message %1\n\nKeep this position and selection.").arg(i)}};
    client->inspectionReady({},session.name,{{"tracked",true},{"conversation_id",session.conversationId},{"phase","working"},{"activity","busy"},{"cursor",30},{"events",events},
        {"session_usage",QJsonObject{{"context",QJsonObject{{"used",139000},{"limit",258400}}}}}});
    auto *button=window.findChild<QPushButton *>("activityMarkRead");QVERIFY(button);QVERIFY(button->isVisible());QVERIFY(!button->icon().isNull());
    QCOMPARE(static_cast<ToolbarChip *>(button)->tone(),ChipTone::Accent);
    auto *browser=window.findChild<ActivityView *>("mainActivity")->browser();auto *composer=window.findChild<MessageComposer *>("messageComposer");
    setComposerText(composer, "Unsent draft");QTRY_VERIFY(!window.findChild<ActivityView *>("mainActivity")->renderPending());
    const auto initialGeometry=browser->geometry();
    browser->setTextCursor(browser->document()->find("Read message 1"));browser->verticalScrollBar()->setValue(0);QTest::qWait(20);
    const auto selection=browser->textCursor().selectedText();QVERIFY(!selection.isEmpty());
    const auto geometry=browser->geometry();const auto revision=browser->document()->revision();
    auto *context=window.findChild<QPushButton *>("activityContext");
    auto *latest=window.findChild<ActivityView *>("mainActivity")->jumpButton();QVERIFY(latest->isVisible());
    const auto bounds=[&](QWidget *widget){return QRect(widget->mapTo(&window,QPoint()),widget->size());};
    // Jump to latest shares the existing toolbar and cannot cover a message.
    QCOMPARE(latest->parentWidget(),composer->toolbar());QVERIFY(latest->text().isEmpty());QVERIFY(!latest->icon().isNull());
    QVERIFY(latest->toolTip().contains("Jump to latest"));QVERIFY(bounds(latest).top()>bounds(browser).bottom());
    QVERIFY(qAbs(bounds(latest).center().y()-bounds(context).center().y())<=1);
    QVERIFY(qAbs(bounds(button).center().y()-bounds(context).center().y())<=1);
    QVERIFY(bounds(button).top()>bounds(browser).bottom());QVERIFY(bounds(button).right()<bounds(context).left());
    QCOMPARE(browser->geometry(),initialGeometry);
    QVERIFY(bounds(button).right()<bounds(context).left());
    QCOMPARE(button->height(),24);
    QVERIFY(qAbs(bounds(context).right()-bounds(composer).right())<=2);
    for(int poll=0;poll<3;++poll){window.setFleet(state);QVERIFY(button->isVisible());}
    for(auto *filter:window.findChildren<QPushButton *>("sessionFilter"))if(filter->property("filter")=="attention")filter->click();
    const auto preview=qEnvironmentVariable("HGS_ATTENTION_PREVIEW");
    if(!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/mark-read-"+theme+".png"));}
    QSignalSpy marks(&window,&SessionsWindow::attentionMarksChanged);button->click();QCOMPARE(marks.size(),1);QVERIFY(button->isHidden());
    QCoreApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
    QCOMPARE(browser->geometry(),geometry);QCOMPARE(browser->document()->revision(),revision);QCOMPARE(browser->verticalScrollBar()->value(),0);
    QCOMPARE(browser->textCursor().selectedText(),selection);QCOMPARE(composer->editor()->toPlainText(),QString("Unsent draft"));
    QVERIFY(!latest->isVisible() || bounds(composer->toolbar()).contains(bounds(latest)));
    auto *list=window.findChild<SessionList *>("sessionList");QVERIFY(list->currentItem()->data(SessionRoles::Key).toString().contains(session.name));
    window.setFleet(state);QVERIFY(button->isHidden());
    session.phase="input";session.activity="waiting";session.attentionId="new-question";state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);QVERIFY(button->isVisible());
    button->click();QVERIFY(button->isHidden());
    // Manual reminders remain actionable even while the machine is offline.
    QVERIFY(state.setReviewLater({},session,true));QSettings().setValue("attention/sessionMarks",QJsonDocument(state.attentionMarks()).toJson(QJsonDocument::Compact));
    box.ok=false;state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);QVERIFY(button->isVisible());button->click();QVERIFY(button->isHidden());
    QTest::qWait(20);const auto beforeJump=browser->geometry();
    latest->click();QTest::qWait(20);QVERIFY(latest->isHidden());QCOMPARE(browser->geometry(),beforeJump);
    QCOMPARE(browser->verticalScrollBar()->value(),browser->verticalScrollBar()->maximum());
}

void TestSessionsWindow::fileDropsAcrossSessionPanelKeepDraftAndTarget()
{
    QTemporaryDir temporary;QVERIFY(temporary.isValid());QFile file(temporary.filePath("report #1.pdf"));
    QVERIFY(file.open(QIODevice::WriteOnly));file.write("%PDF-1.4\nA test report");file.close();
    QMimeData mime;mime.setUrls({QUrl::fromLocalFile(file.fileName())});
    const QJsonObject parent{{"tracked",true},{"conversation_id","conversation-one"},{"run_id","run-one"},
        {"subagents",QJsonObject{{"child-1",QJsonObject{{"name","Review"},{"state","working"}}}}}};
    const QJsonObject childDetails{{"parent_conversation_id","conversation-one"},{"conversation_id","child-one"},
        {"run_id","run-one"},{"send_supported",true}};
    SessionsWindow window(programInspecting("file-drop-hgs",parent,childDetails));window.resize(1100,760);window.setFleet(fleet());window.show();window.showSession({},"codex/hgs/dashboard");
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer, "Keep every word of this draft");
    auto cursor=composer->editor()->textCursor();cursor.select(QTextCursor::Document);composer->editor()->setTextCursor(cursor);
    auto *panel=window.findChild<QWidget *>("sessionDetailPanel");auto *overlay=window.findChild<QWidget *>("sessionFileDropOverlay");QVERIFY(panel);QVERIFY(overlay);
    auto *browser=window.findChild<ActivityView *>("mainActivity")->browser();
    QSignalSpy sent(composer,&MessageComposer::sendRequested);int count=0;
    for(auto *target:QList<QWidget *>{panel,window.findChild<QLabel *>("detailTitle"),browser->viewport()}) {
        QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(target,&enter);
        QVERIFY(enter.isAccepted());QVERIFY(overlay->isVisible());QCOMPARE(overlay->geometry(),panel->rect());
        const auto preview=qEnvironmentVariable("HGS_DROP_PREVIEW");if(!preview.isEmpty()&&count==0){QDir().mkpath(preview);QVERIFY(window.grab().save(preview+"/session-file-drop.png"));}
        QDropEvent drop(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(target,&drop);
        QVERIFY(drop.isAccepted());QVERIFY(overlay->isHidden());QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),++count);
        QVERIFY(composer->editor()->toPlainText().contains("Keep every word of this draft"));
    }
    auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");
    window.findChild<QPushButton *>("toggleInspector")->setChecked(true);window.findChild<QTabWidget *>("sessionInspector")->setCurrentIndex(1);
    for(auto *target:QList<QWidget *>{window.findChild<QTextBrowser *>("sessionInfo")->viewport()}) {
        if(target==tabs->widget(1))tabs->setCurrentIndex(1);
        QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(target,&enter);QVERIFY(enter.isAccepted());
        QDropEvent drop(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(target,&drop);
        QVERIFY(drop.isAccepted());QCOMPARE(tabs->currentIndex(),0);QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),++count);
    }
    QCOMPARE(sent.size(),0);QVERIFY(QFileInfo::exists(file.fileName()));
    QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&enter);QVERIFY(enter.isAccepted());
    window.showSession("mac","claude/infra/review");
    QDropEvent stale(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&stale);
    QVERIFY(!stale.isAccepted());QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),0);
    window.showSession({},"codex/hgs/dashboard");QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),count);
    QMimeData web;web.setUrls({QUrl("https://example.test/file.pdf")});
    QDragEnterEvent remote(QPoint(10,10),Qt::CopyAction,&web,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&remote);QVERIFY(!remote.isAccepted());
    // A visible subagent composer owns the panel's drop; the parent draft stays intact.
    auto *client=window.findChild<HgsClient *>();QTest::qWait(60);
    client->inspectionReady({},"codex/hgs/dashboard",parent);
    window.findChild<QTextBrowser *>("subagents")->anchorClicked(QUrl("hgs-agent:child-1"));
    client->subagentInspectionReady({},"codex/hgs/dashboard","child-1",{},childDetails);
    auto *child=window.findChild<MessageComposer *>("subagentComposer");QVERIFY(child->isInputVisible());setComposerText(child, "Child draft");
    QDragEnterEvent childEnter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&childEnter);QVERIFY(childEnter.isAccepted());
    QDropEvent childDrop(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&childDrop);QVERIFY(childDrop.isAccepted());
    QTRY_COMPARE(child->findChildren<QWidget *>("attachmentRow").size(),1);QVERIFY(child->editor()->toPlainText().contains("Child draft"));
    QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),count);QCOMPARE(sent.size(),0);
}

void TestSessionsWindow::terminalDropKeepsActivityDraftAndDoesNotSubmit()
{
    const auto program=m_dir->filePath("terminal-drop-hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport os,runpy,sys\na=sys.argv[1:]\nif a and a[0].startswith('@'):a=a[1:]\n"
        "if a[:1]==['a']:\n if os.name!='nt':\n  import tty;tty.setraw(0)\n"
        " sys.stdout.write('\\x1b[?2004h');sys.stdout.flush();sys.stdin.buffer.read();sys.exit(0)\n"
        "sys.argv[0]="+FakeCli::pythonString(script())+";runpy.run_path(sys.argv[0],run_name='__main__')\n");
    fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    const auto path=m_dir->filePath("report's draft.pdf");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
    // The dropped file is a real one of this computer, in the form of the GUI's platform.
    auto state=fleet();auto own=state.local();own.metrics.remove("platform");state.setLocal(own,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(program));window.setFleet(state);window.resize(1100,760);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer, "Keep Activity draft");
    auto *terminal=window.findChild<TerminalView *>();auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");
    tabs->setCurrentWidget(terminal);QTRY_VERIFY(terminal->isConnected());QTest::qWait(150);
    QSignalSpy output(terminal->terminalScreen(),&TerminalScreen::output);QSignalSpy sent(composer,&MessageComposer::sendRequested);
    QMimeData mime;mime.setUrls({QUrl::fromLocalFile(path)});
    auto *panel=window.findChild<QWidget *>("sessionDetailPanel");
    QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&enter);QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&drop);QVERIFY(drop.isAccepted());
    QCOMPARE(tabs->currentWidget(),terminal);QCOMPARE(composer->editor()->toPlainText(),QString("Keep Activity draft"));
    QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),0);QCOMPARE(sent.size(),0);
    QByteArray pasted;for(const auto &item:output)pasted+=item.first().toByteArray();
#ifdef Q_OS_WIN
    // This Windows computer reads native paths (terminalDropQuotesPathsForTheMachine).
    QVERIFY(pasted.contains(('"'+QDir::toNativeSeparators(path)+'"').toUtf8()));
#else
    QVERIFY(pasted.contains("report'\\''s draft.pdf"));
#endif
    QVERIFY(!pasted.contains('\r'));QVERIFY(!pasted.contains('\n'));
    output.clear();QDragEnterEvent second(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&second);
    tabs->setCurrentIndex(0);QDropEvent stale(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&stale);
    QVERIFY(!stale.isAccepted());
#ifdef Q_OS_WIN
    // Nothing reaches the terminal but the focus reports a pseudoconsole asks for (DECSET 1004).
    QByteArray rest;for(const auto &item:output)rest+=item.first().toByteArray();
    rest.replace("\x1b[I","").replace("\x1b[O","");QCOMPARE(rest,QByteArray());
#else
    QCOMPARE(output.size(),0);
#endif
    QCOMPARE(composer->findChildren<QWidget *>("attachmentRow").size(),0);
}

void TestSessionsWindow::sessionAttentionMenuPersistsWithoutSelection()
{
    auto state = fleet(); auto box = state.local();
    box.sessions[0].conversationId = "review-conversation"; box.sessions[0].runId = "run";
    box.sessions[0].phase = "approval"; box.sessions[0].activity = "waiting";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList");
    auto *menu = window.findChild<QMenu *>("sessionContextMenu");
    QSignalSpy marks(&window, &SessionsWindow::attentionMarksChanged);
    list->setCurrentRow(1);
    const auto open = [&] {
        menu->hide(); const auto point = list->visualItemRect(list->item(0)).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, list->viewport()->mapToGlobal(point));
        QApplication::sendEvent(list->viewport(), &event);
    };
    open(); QVERIFY(!menu->findChild<QAction *>("contextReviewLater"));
    menu->findChild<QAction *>("contextMarkRead")->trigger();
    QCOMPARE(list->currentRow(), 1); QCOMPARE(marks.size(), 1);
    QVERIFY(!list->item(0)->data(SessionRoles::Attention).toBool());
    open(); QVERIFY(!menu->findChild<QAction *>("contextMarkRead"));
    menu->findChild<QAction *>("contextReviewLater")->trigger();
    QCOMPARE(list->currentRow(), 1); QCOMPARE(marks.size(), 2);
    QVERIFY(list->item(0)->data(SessionRoles::ReviewLater).toBool());
    window.setFleet(state); // Native polls do not contain UI reminders.
    QVERIFY(list->item(0)->data(SessionRoles::ReviewLater).toBool());
    open(); QVERIFY(menu->findChild<QAction *>("contextMarkRead")->isEnabled());
    QVERIFY(!menu->findChild<QAction *>("contextReviewLater"));
    menu->findChild<QAction *>("contextMarkRead")->trigger();
    QCOMPARE(list->currentRow(), 1); QCOMPARE(marks.size(), 3);
    QVERIFY(!list->item(0)->data(SessionRoles::ReviewLater).toBool());
    QVERIFY(!list->item(0)->data(SessionRoles::Attention).toBool());
    window.setFleet(state); QVERIFY(!list->item(0)->data(SessionRoles::Attention).toBool());
    open(); menu->findChild<QAction *>("contextReviewLater")->trigger(); menu->hide();
    SessionsWindow reopened(program()); reopened.setFleet(state); reopened.show();
    QVERIFY(reopened.findChild<QListWidget *>("sessionList")->item(0)->data(SessionRoles::ReviewLater).toBool());
}

void TestSessionsWindow::sessionAttentionMenuSurvivesRoutinePoll()
{
    auto state = fleet(); auto box = state.local(); auto &session = box.sessions[0];
    session.conversationId = "poll-conversation"; session.runId = "run";
    session.phase = "input"; session.activity = "waiting";
    session.attentionId = "question-1"; session.lastEventAt = 100;
    state.setLocal(box, 1000);
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList");
    auto *menu = window.findChild<QMenu *>("sessionContextMenu");
    QSignalSpy marks(&window, &SessionsWindow::attentionMarksChanged);
    list->setCurrentRow(1);
    const auto open = [&] {
        menu->hide(); const auto point = list->visualItemRect(list->item(0)).center();
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, list->viewport()->mapToGlobal(point));
        QApplication::sendEvent(list->viewport(), &event);
    };
    open(); auto *read = menu->findChild<QAction *>("contextMarkRead"); QVERIFY(read);
    session.lastEventAt = 101;
    session.subagents = {{"worker", QJsonObject{{"state", "working"}, {"summary", "Another tool"}}}};
    state.setLocal(box, 2000); window.setFleet(state); read->trigger();
    QCOMPARE(marks.size(), 1); QCOMPARE(list->currentRow(), 1);
    QVERIFY(!list->item(0)->data(SessionRoles::Attention).toBool());
    session.lastEventAt = 102; state.setLocal(box, 3000); window.setFleet(state);
    QVERIFY(!list->item(0)->data(SessionRoles::Attention).toBool());
    session.attentionId = "question-2"; state.setLocal(box, 4000); window.setFleet(state);
    open(); read = menu->findChild<QAction *>("contextMarkRead"); QVERIFY(read);
    session.attentionId = "question-3"; state.setLocal(box, 5000); window.setFleet(state);
    read->trigger(); QCOMPARE(marks.size(), 1);
    QVERIFY(list->item(0)->data(SessionRoles::Attention).toBool());
    QVERIFY(window.findChild<QLabel *>("notice")->text().contains("A new reply or question arrived"));
    menu->hide();
}

void TestSessionsWindow::sessionContextMenuGuardsIdentityAndChanges()
{
    auto state = fleet(); auto box = state.local(); box.sessions[0].activity = "idle"; box.sessions[0].phase = "idle"; box.sessions[0].created = 100;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList"); auto *menu = window.findChild<QMenu *>("sessionContextMenu");
    auto show = [&]() { menu->hide(); list->setCurrentRow(0); QTest::keyClick(list, Qt::Key_Menu); };
    auto action = [&](const char *name) { return menu->findChild<QAction *>(name); };
    auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone), copies(&window, &SessionsWindow::copySessionCommandRequested);
    show(); QVERIFY(action("contextChangeSession")->isEnabled()); list->setCurrentRow(1);
    action("contextChangeSession")->trigger(); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QString("pause\ncodex/hgs/dashboard")); QCOMPARE(list->currentRow(), 0);
    show(); auto *pause = action("contextChangeSession"); box.sessions[0].activity = "busy";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); pause->trigger(); QTest::qWait(30); QCOMPARE(writes.size(), 1);
    box.sessions[0].activity = "idle"; state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    show(); pause = action("contextChangeSession"); box.sessions[0].state = "paused";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); pause->trigger(); QTest::qWait(30); QCOMPARE(writes.size(), 1);
    // A removed row cannot redirect a saved QAction to the newly selected row.
    show(); auto *copy = action("contextCopyCommand"); const auto removed = box.sessions.takeFirst();
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); copy->trigger(); QCOMPARE(copies.size(), 0);
    QVERIFY(window.findChild<QLabel *>("notice")->text().contains("no longer available"));
    box.sessions.prepend(removed); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    show(); copy = action("contextCopyCommand"); box.sessions[0].created = 200;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); copy->trigger(); QCOMPARE(copies.size(), 0);
    // Polling inside a confirmation dialog must also preserve the target.
    show(); QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        box.sessions.removeFirst(); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
        dialog->button(QMessageBox::Yes)->click();
    });
    action("contextTerminateSession")->trigger(); QTest::qWait(30); QCOMPARE(writes.size(), 1);
}

void TestSessionsWindow::sessionContextMenuSavedAndArchive()
{
    auto state = fleet(); auto box = state.local(); auto older = box.sessions[2];
    older.state = "archived"; older.archiveId = "older"; older.archivedAt = 100; older.cwd = "/saved/folder";
    auto recent = older; recent.archiveId = "recent"; recent.archivedAt = 200;
    box.sessions.append(older); box.sessions.append(recent); state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, "codex/website/navigation");
    auto *list = window.findChild<QListWidget *>("sessionList"); auto *menu = window.findChild<QMenu *>("sessionContextMenu");
    auto show = [&]() { menu->hide(); QTest::keyClick(list, Qt::Key_Menu); };
    auto action = [&](const char *name) { return menu->findChild<QAction *>(name); };
    QSignalSpy copies(&window, &SessionsWindow::copySessionCommandRequested);
    auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
    show(); QCOMPARE(action("contextChangeSession")->text(), QString("Resume session")); QVERIFY(action("contextArchiveSession"));
    QCOMPARE(action("contextTerminateSession")->text(), QString("Forget saved session…"));
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLineEdit *>("renameSessionName")->text(), QString("navigation")); dialog->reject();
    });
    action("contextRenameSession")->trigger(); QCOMPARE(writes.size(), 0); menu->hide();
    for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "archived") button->click();
    list->setCurrentRow(0);
    show(); QVERIFY(!action("contextOpenTerminal")); QVERIFY(!action("contextArchiveSession"));
    QCOMPARE(action("contextCopyCommand")->text(), QString("Copy restore command"));
    QCOMPARE(action("contextChangeSession")->text(), QString("Restore session")); QVERIFY(!action("contextChangeSession")->isEnabled()); // Saved name collision.
    list->setCurrentRow(1); action("contextCopyCommand")->trigger(); QCOMPARE(copies.size(), 1);
    QCOMPARE(copies[0][1].toString(), QString("codex/website/navigation")); QCOMPARE(copies[0][2].toString(), QString("recent"));
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); QVERIFY(dialog); dialog->button(QMessageBox::Yes)->click();
    });
    list->setCurrentRow(1); action("contextTerminateSession")->trigger(); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QString("kill\ncodex/website/navigation\n--archive\nrecent"));
}

void TestSessionsWindow::sessionRowsHaveNoHoverPopup()
{
    auto state = fleet(); auto box = state.local();
    auto &session = box.sessions[0];
    session.cwd = "/workspace/" + QString(6000, 'p');
    session.gitBranch = "feature/" + QString(6000, 'b');
    session.activitySummary = "<img src='file:///private'> " + QString(12000, 'x');
    session.activityDetail = QString(12000, 'y');
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.showSession({}, session.name);
    auto *list = window.findChild<QListWidget *>("sessionList");
    QVERIFY(list->currentItem());
    QVERIFY(list->currentItem()->toolTip().isEmpty());
    const auto accessible = list->currentItem()->data(Qt::AccessibleTextRole).toString();
    QVERIFY(accessible.size() < 1200); QVERIFY(accessible.count('\n') <= 11);
    QVERIFY(accessible.contains(session.name));
    window.setFleet(state);
    QVERIFY(list->currentItem()->toolTip().isEmpty());
}

void TestSessionsWindow::buttonFocusFollowsKeyboardAndNotClicks()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); QTest::qWait(50);
    auto *toggle = window.findChild<QPushButton *>("toggleInspector");
    QTest::mouseClick(toggle, Qt::LeftButton);
    QCOMPARE(toggle->property("keyboardFocus").toBool(), false);
    toggle->setFocus(Qt::TabFocusReason); QTest::keyClick(toggle, Qt::Key_Tab);
    auto *focused = qobject_cast<QPushButton *>(QApplication::focusWidget());
    QVERIFY(focused); QVERIFY(focused->property("keyboardFocus").toBool());
    QTest::mouseClick(focused, Qt::LeftButton);
    QVERIFY(!focused->property("keyboardFocus").toBool());
    // Pressing a key on an already focused button restores its keyboard ring.
    QTest::keyClick(focused, Qt::Key_Shift); QVERIFY(!focused->property("keyboardFocus").toBool());
    QTest::keyClick(focused, Qt::Key_Right); QVERIFY(focused->property("keyboardFocus").toBool());

    // Refresh disables itself while connecting. Qt then transfers focus to
    // the next enabled button, even though the user has not pressed Tab.
    QWidget controls; QVBoxLayout layout(&controls);
    QPushButton refresh("Refresh"), details("Details");
    layout.addWidget(&refresh); layout.addWidget(&details);
    connect(&refresh, &QPushButton::clicked, &refresh, [&] { refresh.setEnabled(false); });
    controls.show(); controls.activateWindow(); QTest::qWait(20);
    QTest::mouseClick(&refresh, Qt::LeftButton);
    QVERIFY(details.hasFocus()); QVERIFY(!details.property("keyboardFocus").toBool());
    refresh.setEnabled(true); refresh.setFocus(Qt::MouseFocusReason);
    QTest::keyClick(&refresh, Qt::Key_Space);
    QVERIFY(details.hasFocus()); QVERIFY(details.property("keyboardFocus").toBool());
}

void TestSessionsWindow::inspectorPreservesWorkspaceAndReportsTasks()
{
    QSettings().setValue("processes/enabled",true);
    SessionsWindow window(program()); window.setFleet(fleet()); window.resize(1120, 760); window.show(); QTest::qWait(80);
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
    auto *inspector = window.findChild<QTabWidget *>("sessionInspector");
    auto *toggle = window.findChild<QPushButton *>("toggleInspector");
    auto *panel = window.findChild<QFrame *>("sessionInspectorPanel");
    auto *splitter = window.findChild<QSplitter *>("sessionWorkSplitter");
    auto *editor = window.findChild<MessageComposer *>("messageComposer")->findChild<QPlainTextEdit *>("messageInput");
    QVERIFY(tabs && inspector && toggle && panel && splitter && editor);
    QCOMPARE(tabs->count(), 4); QCOMPARE(tabs->tabText(0), QString("Activity")); QCOMPARE(tabs->tabText(1), QString("Terminal"));
    QCOMPARE(tabs->tabText(3),QString("Processes"));QVERIFY(tabs->isTabVisible(3));
    QVERIFY(tabs->isTabVisible(1)); QVERIFY(!tabs->isTabVisible(2));
    QVERIFY(panel->isHidden()); QCOMPARE(tabs->cornerWidget(), toggle);
    editor->setPlainText("Keep this draft"); const int originalWidth = tabs->width();
    QTest::mouseClick(toggle, Qt::LeftButton); QTest::qWait(40);
    QVERIFY(inspector->isVisible()); QVERIFY(tabs->width() < originalWidth); QVERIFY(panel->width() >= 240);
    QVERIFY(!tabs->cornerWidget()); QCOMPARE(inspector->cornerWidget(), toggle);
    QCOMPARE(tabs->currentIndex(), 0); QCOMPARE(editor->toPlainText(), QString("Keep this draft"));
    QJsonObject data{{"tracked", true}, {"run_id", "run-1"}, {"conversation_id", "conversation-one"},
        {"subagent_source", "hooks"}, {"subagents", QJsonObject{{"review", QJsonObject{{"name", "Reviewer"}, {"state", "working"}}}}},
        {"task_lists", QJsonObject{
            {"main", QJsonObject{{"run_id", "run-1"}, {"items", QJsonArray{
                QJsonObject{{"title", "Inspect <source> & hooks"}, {"status", "completed"}},
                QJsonObject{{"title", "Implement right panel"}, {"status", "in_progress"}}}}}},
            {"agent:review", QJsonObject{{"run_id", "older-run"}, {"items", QJsonArray{
                QJsonObject{{"title", "Verify boundaries"}, {"status", "pending"}}}}}}
        }}};
    auto *client = window.findChild<HgsClient *>(); client->inspectionReady({}, "codex/hgs/dashboard", data);
    auto *roster = window.findChild<QTextBrowser *>("subagents"); const auto text = roster->toPlainText();
    QVERIFY(text.contains("Inspect <source> & hooks")); QVERIFY(text.contains("1 of 2 completed"));
    QVERIFY(text.contains("In progress")); QVERIFY(text.contains("Reviewer")); QVERIFY(text.contains("Last recorded"));
    inspector->setCurrentIndex(2); QCOMPARE(tabs->currentIndex(), 0);
    splitter->setSizes({420, 310});
    QTest::mouseClick(toggle, Qt::LeftButton); QTest::qWait(40);
    QVERIFY(panel->isHidden()); QVERIFY(!toggle->isChecked()); QCOMPARE(editor->toPlainText(), QString("Keep this draft"));
    QCOMPARE(tabs->cornerWidget(), toggle); QVERIFY(!inspector->cornerWidget()); QVERIFY(toggle->isVisible());
    tabs->setCurrentIndex(1); QTest::mouseClick(toggle, Qt::LeftButton); QCOMPARE(tabs->currentIndex(), 1);
    QCOMPARE(inspector->currentIndex(), 2);
    data["processes"]=QJsonObject{{"active_count",1},{"items",QJsonArray{
        QJsonObject{{"id","build"},{"command","cargo build --release"},{"description","Build the updated CLI"},{"owner","main"},{"cwd","/workspace/zerus"},{"status","running"},{"started_at",QDateTime::currentSecsSinceEpoch()-23}},
        QJsonObject{{"id","tests"},{"command","python3 tests/test_dsh_native.py -v"},{"owner","main"},{"cwd","/workspace/zerus"},{"status","completed"},{"started_at",QDateTime::currentSecsSinceEpoch()-65},{"ended_at",QDateTime::currentSecsSinceEpoch()-12},{"exit_code",0}}
    }}};
    client->inspectionReady({},"codex/hgs/dashboard",data);QCOMPARE(tabs->tabText(3),QString("Processes (1)"));
    tabs->setCurrentIndex(3);QVERIFY(window.findChild<QTreeWidget*>("processList")->isVisible());QCOMPARE(editor->toPlainText(),QString("Keep this draft"));
    if(!qEnvironmentVariable("HGS_PROCESSES_WINDOW_PREVIEW").isEmpty()){
        QTest::mouseClick(toggle,Qt::LeftButton);QTest::qWait(80);QVERIFY(window.grab().save(qEnvironmentVariable("HGS_PROCESSES_WINDOW_PREVIEW")));QTest::mouseClick(toggle,Qt::LeftButton);
    }
    window.close();
    SessionsWindow reopened(program()); reopened.setFleet(fleet()); reopened.show(); QTest::qWait(60);
    auto *restored = reopened.findChild<QTabWidget *>("sessionInspector"); QVERIFY(restored->isVisible()); QCOMPARE(restored->currentIndex(), 2);
    // A smaller window still gives both panels a usable width and no overlay.
    reopened.resize(960, 650); QTest::qWait(40);
    auto *main = reopened.findChild<QTabWidget *>("sessionDetailTabs"); QVERIFY(main->width() >= 280);
    auto *restoredPanel = reopened.findChild<QFrame *>("sessionInspectorPanel");
    QCOMPARE(main->parentWidget(), restoredPanel->parentWidget());
    QVERIFY(main->geometry().right() < restoredPanel->geometry().left());
    auto *bar=restored->tabBar();
    QCOMPARE(bar->elideMode(),Qt::ElideNone);QVERIFY(!bar->usesScrollButtons());
    restored->setStyleSheet("QTabBar { font-size:24px; } QTabBar::tab { font-size:24px; }");QTest::qWait(50);
    for(int i=0;i<bar->count();++i)QVERIFY2(bar->tabRect(i).width()>=bar->fontMetrics().horizontalAdvance(QString(bar->tabText(i)).replace("&&","&"))+12,qPrintable(QString("%1 rect=%2 text=%3").arg(bar->tabText(i)).arg(bar->tabRect(i).width()).arg(bar->fontMetrics().horizontalAdvance(QString(bar->tabText(i)).replace("&&","&")))));
    for(int i=0;i<bar->count();++i){QVERIFY(bar->tabRect(i).left()>=0);QVERIFY(bar->tabRect(i).right()<bar->width());}
    QVERIFY(bar->geometry().right()<restored->cornerWidget()->geometry().left());
    // Larger tabs raise the panel's minimum width; a slow runner may apply that relayout after a fixed wait.
    QTRY_VERIFY2(restoredPanel->mapTo(&reopened,restoredPanel->rect().topRight()).x()<reopened.width(),
        qPrintable(QString("panel right=%1 window width=%2 window minimum=%3").arg(restoredPanel->mapTo(&reopened,restoredPanel->rect().topRight()).x())
            .arg(reopened.width()).arg(reopened.minimumSizeHint().width())));
    const auto inspectorPreview=qEnvironmentVariable("HGS_INSPECTOR_PREVIEW");if(!inspectorPreview.isEmpty())QVERIFY(reopened.grab().save(inspectorPreview));
    reopened.showSession({}, "kimi/docs/research"); QTest::qWait(50);
    QVERIFY(!reopened.findChild<QTextBrowser *>("subagents")->toPlainText().contains("Implement right panel"));
}

void TestSessionsWindow::detailsPollingPreservesReadingPosition()
{
    SessionsWindow window(program()); window.resize(1150, 650); window.setFleet(fleet());
    // The window's own inspections answer with the base fixture's shorter details in
    // the same conversation. One arriving while the test waits replaces the document
    // under test, and the kept position is clamped to its end. Inspect only on
    // selection, then wait for an inspection of the test's own: HgsClient runs one per
    // session, so once its reply is in, none is pending and no poll follows.
    window.setDetailsPollForTests(500, 24 * 60 * 60 * 1000); window.show();
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspections(client, &HgsClient::inspectionReady);
    window.showSession({}, "codex/hgs/dashboard");
    auto *inspector = window.findChild<QTabWidget *>("sessionInspector");
    window.findChild<QPushButton *>("toggleInspector")->setChecked(true); inspector->setCurrentIndex(1);
    auto *info = window.findChild<QTextBrowser *>("sessionInfo");
    auto *scroll = info->verticalScrollBar();
    qsizetype issued = -1;
    QTRY_VERIFY(issued >= 0 || (client->requestInspection({}, "codex/hgs/dashboard") && (issued = inspections.size(), true)));
    QTRY_VERIFY(std::any_of(inspections.cbegin() + issued, inspections.cend(), [](const QList<QVariant> &reply) {
        return reply[0].toString().isEmpty() && reply[1].toString() == "codex/hgs/dashboard" && reply[3].toString().isEmpty();
    }));
    QJsonObject totals{{"total", 4280764}, {"input", 4169472}, {"uncached_input", 64},
        {"cache_read", 3770489}, {"cache_write", 398919}, {"output", 111292},
        {"reasoning", 62310}, {"web_searches", 0}, {"web_fetches", 0}};
    QJsonObject usage{{"status", "ok"}, {"totals", totals}, {"requests", 32},
        {"context", QJsonObject{{"used", 233246}}}};
    QJsonObject details{{"tracked", true}, {"conversation_id", "conversation-one"}, {"run_id", "run-one"},
        {"cwd", "/work/project"}, {"git_root", "/work/project"}, {"git_branch", "main"},
        {"last_event_at", 1791266000}, {"session_usage", usage}};
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    QTRY_VERIFY(scroll->maximum() > 100);
    auto selected = info->document()->find("Session usage"); QVERIFY(!selected.isNull());
    info->setTextCursor(selected);
    scroll->setValue(scroll->maximum() - 40); const int position = scroll->value();
    const int revision = info->document()->revision();
    // Identical polls must not replace the document at all.
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    QCOMPARE(info->document()->revision(), revision); QCOMPARE(scroll->value(), position);
    for (int i = 0; i < 4; ++i) {
        details["last_event_at"] = 1791266001 + i;
        totals["total"] = 10000000 + i; usage["totals"] = totals;
        usage["requests"] = 100 + i; details["session_usage"] = usage;
        client->inspectionReady({}, "codex/hgs/dashboard", details);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCOMPARE(scroll->value(), position);
        QCOMPARE(info->textCursor().selectedText(), QString("Session usage"));
        QVERIFY(info->toPlainText().simplified().contains(QLocale().toString(10000000 + i).simplified()));
        QVERIFY(info->updatesEnabled());
    }
    // Polls of a hidden Details tab and closing/reopening the inspector are
    // still updates to the same reading context.
    inspector->setCurrentIndex(0);
    details["last_event_at"] = 1791266010; client->inspectionReady({}, "codex/hgs/dashboard", details);
    inspector->setCurrentIndex(1); QTest::qWait(20); QCOMPARE(scroll->value(), position);
    auto *toggle = window.findChild<QPushButton *>("toggleInspector");
    toggle->setChecked(false);
    details["last_event_at"] = 1791266011; client->inspectionReady({}, "codex/hgs/dashboard", details);
    toggle->setChecked(true); QTest::qWait(20); QCOMPARE(scroll->value(), position);
    // A different conversation in the same terminal starts at the top.
    details["conversation_id"] = "conversation-two";
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    QCOMPARE(scroll->value(), 0); QVERIFY(!info->textCursor().hasSelection());
    scroll->setValue(scroll->maximum()); QVERIFY(scroll->value() > 0);
    window.showSession("mac", "claude/infra/review");
    QCOMPARE(scroll->value(), 0); QVERIFY(!info->textCursor().hasSelection());
}

void TestSessionsWindow::nativeGoalIsIndependentOfTurnState()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); QTest::qWait(80);
    auto *client = window.findChild<HgsClient *>();
    auto *goalButton = window.findChild<QPushButton *>("sessionGoal");
    QVERIFY(goalButton->isHidden());
    QJsonObject goal{{"id", "goal-one"}, {"objective", "Finish <migration> & verify restore"}, {"status", "active"},
        {"tokens_used", 1000}, {"token_budget", 9000}, {"time_used_seconds", 154800}};
    QJsonObject data{{"tracked", true}, {"conversation_id", "conversation-one"}, {"activity", "idle"}, {"goal", goal}};
    client->inspectionReady({}, "codex/hgs/dashboard", data);
    QVERIFY(goalButton->isVisible()); QVERIFY(goalButton->text().contains("Pursuing goal"));
    QVERIFY(goalButton->text().contains("1d 19h"));
    QTest::mouseClick(goalButton, Qt::LeftButton);
    auto *inspector = window.findChild<QTabWidget *>("sessionInspector");
    QVERIFY(inspector->isVisible()); QCOMPARE(inspector->currentIndex(), 0);
    auto *roster = window.findChild<QTextBrowser *>("subagents");
    QVERIFY(roster->toPlainText().contains("Finish <migration> & verify restore"));
    QVERIFY(roster->toPlainText().contains("tokens"));
    for (const auto &state : {"paused", "blocked", "usage_limited", "budget_limited", "complete"}) {
        goal["status"] = state; data["goal"] = goal; client->inspectionReady({}, "codex/hgs/dashboard", data);
        QVERIFY(goalButton->isVisible()); QVERIFY(!goalButton->text().contains("Pursuing goal"));
    }
    data["goal"] = QJsonValue::Null; data["goal_source"] = "unavailable";
    client->inspectionReady({}, "codex/hgs/dashboard", data);
    QVERIFY(goalButton->isHidden()); QVERIFY(roster->toPlainText().contains("Goal data is unavailable"));
    window.showSession({}, "kimi/docs/research"); QTest::qWait(40);
    QVERIFY(goalButton->isHidden()); QVERIFY(!roster->toPlainText().contains("Finish <migration>"));
}

void TestSessionsWindow::fileReferencesKeepMachineAndWorkspace()
{
    // Polls during the first dialog re-inspect the session; they must keep its workspace.
    const auto fixture = programInspecting("file-reference-hgs", {{"tracked", true}, {"conversation_id", "conversation-one"},
        {"events", QJsonArray{}}, {"cursor", 0}, {"cwd", "/local/project"}}); QVERIFY(!fixture.isEmpty());
    SessionsWindow window(fixture); window.setFleet(fleet()); window.show(); QTest::qWait(80);
    auto *client = window.findChild<HgsClient *>();
    auto *activity = window.findChild<ActivityView *>("mainActivity");
    QSignalSpy copied(&window, &SessionsWindow::copyTextRequested);
    QSignalSpy ssh(&window, &SessionsWindow::machineSshRequested);
    client->inspectionReady({}, "codex/hgs/dashboard", {{"cwd", "/local/project"}, {"conversation_id", "conversation-one"}});
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("sessionFileDialog"); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLineEdit *>("sessionFilePath")->text(), QString("/local/project/docs/README.md"));
        QVERIFY(dialog->findChild<QPushButton *>("openSessionFile"));
        QVERIFY(!dialog->findChild<QPushButton *>("openSessionFileSsh"));
        auto *copy=dialog->findChild<QPushButton *>("copySessionFilePath");
        QVERIFY(copy->text().isEmpty());QVERIFY(!copy->icon().isNull());
        copy->click();QVERIFY(dialog->isVisible());
        dialog->reject();
    });
    activity->fileReferenceActivated("docs/README.md:12");
    QCOMPARE(copied.takeFirst().at(0).toString(), QString("/local/project/docs/README.md"));
    // A file link in a question's Markdown opens the same dialog.
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("sessionFileDialog"); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLineEdit *>("sessionFilePath")->text(), QString("/local/project/docs/README.md"));
        dialog->reject();
    });
    window.findChild<QuestionCard *>()->fileReferenceActivated("docs/README.md:12");
    QVERIFY(!window.findChild<QDialog *>("sessionFileDialog"));
    window.showSession("mac", "claude/infra/review"); QTest::qWait(50);
    client->inspectionReady("mac", "claude/infra/review", {{"cwd", "/Users/remote/project"}, {"conversation_id", "conversation-one"}});
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("sessionFileDialog"); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLineEdit *>("sessionFilePath")->text(), QString("/Users/remote/project/docs/README.md"));
        QVERIFY(!dialog->findChild<QPushButton *>("openSessionFile"));
        dialog->findChild<QPushButton *>("copySessionFilePath")->click();QVERIFY(dialog->isVisible());
        dialog->findChild<QPushButton *>("openSessionFileSsh")->click();QVERIFY(!dialog->isVisible());
    });
    activity->fileReferenceActivated("docs/README.md");
    QCOMPARE(copied.size(), 1); QCOMPARE(copied.takeFirst().at(0).toString(), QString("/Users/remote/project/docs/README.md"));
    QCOMPARE(ssh.size(), 1); QCOMPARE(ssh.takeFirst().at(0).toString(), QString("mac"));
}

void TestSessionsWindow::remoteFileDownloads_data()
{
    QTest::addColumn<int>("mode");
    QTest::newRow("download-and-open-local-copy") << 0;
    QTest::newRow("download-and-open-directly") << 1;
    QTest::newRow("failure-keeps-dialog-and-existing-copy") << 2;
    QTest::newRow("cancel-save-does-not-download") << 3;
    QTest::newRow("closing-dialog-keeps-requested-download-and-open") << 4;
    // A downloaded program is shown in its folder rather than run.
    QTest::newRow("download-and-open-program") << 5;
    QTest::newRow("automatic-name-collision") << 6;
    QTest::newRow("automatic-download-failure") << 7;
}

void TestSessionsWindow::remoteFileDownloads()
{
    QFETCH(int, mode);
    const auto nativeDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restoreDialogs = qScopeGuard([=] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogs); });
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const bool automatic = mode == 1 || mode >= 4;
    const QString path = mode == 5 ? "/remote/project/deploy.sh" : "/remote/project/résumé ' $(literal).png";
    const auto cache = directory.filePath("Zerus Files"); QSettings().setValue("downloads/folder", cache);
    const auto destination = automatic ? QDir(cache).filePath(mode == 6 ? "résumé ' $(literal) (1).png" : QFileInfo(path).fileName())
                                       : directory.filePath("local résumé's copy.png");
    if (mode == 6) {
        QVERIFY(QDir().mkpath(cache)); QFile existing(QDir(cache).filePath(QFileInfo(path).fileName()));
        QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("existing copy");
    }
    if (mode == 2) { QFile file(destination); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("previous copy"); }
    const auto fixture = m_dir->filePath("file-download-hgs");
    QFile original(script()); QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray answer = R"PY(elif c=='download':
 import pathlib,time
 open(__file__+'.calls','a').write(json.dumps(sys.argv[1:])+'\n')
 time.sleep(.1)
 if pathlib.Path(__file__+'.fail').exists():
  print('Permission denied (publickey).',file=sys.stderr);sys.exit(1)
 target=a[a.index('--output')+1];path=a[a.index('--path')+1]
 pathlib.Path(target).write_bytes(b'synthetic image')
 print(json.dumps(dict(host=sys.argv[1][1:],path=path,local_path=target,bytes=15)))
)PY";
    QVERIFY(FakeCli::install(fixture, original.readAll().replace("if False:pass\n", "if False:pass\n" + answer)));
    QFile::remove(fixture + ".calls"); QFile::remove(fixture + ".fail");
    if (mode == 2 || mode == 7) { QFile fail(fixture + ".fail"); QVERIFY(fail.open(QIODevice::WriteOnly)); }
    FolderUrlRecorder opened;
    SessionsWindow window(FakeCli::program(fixture)); window.setFleet(fleet()); window.show();
    window.showSession("mac", "claude/infra/review");
    auto *client = window.findChild<HgsClient *>();
    QSignalSpy downloaded(client, &HgsClient::fileDownloaded), failed(client, &HgsClient::fileDownloadFailed);
    QPointer<QDialog> dialog;
    QTimer::singleShot(0, &window, [&] {
        dialog = window.findChild<QDialog *>("sessionFileDialog"); QVERIFY(dialog);
        const auto close = qScopeGuard([&] { if (dialog) dialog->reject(); });
        QVERIFY(opened.urls.isEmpty()); QVERIFY(!QFile::exists(fixture + ".calls"));
        if (mode == 0 && qEnvironmentVariableIsSet("HGS_TEST_FILE_DIALOG_PREVIEW"))
            QVERIFY(dialog->grab().save(qEnvironmentVariable("HGS_TEST_FILE_DIALOG_PREVIEW")));
        auto *download = dialog->findChild<QPushButton *>(automatic ? "downloadOpenSessionFile" : "downloadSessionFile");
        QVERIFY(download); QVERIFY(download->isEnabled());
        if (!automatic) QTimer::singleShot(0, &window, [&] {
            auto *save = window.findChild<QFileDialog *>("sessionFileSaveDialog"); QVERIFY(save);
            if (mode == 3) { save->reject(); return; }
            save->setOption(QFileDialog::DontConfirmOverwrite); save->selectFile(destination);
            QMetaObject::invokeMethod(save, "accept", Qt::DirectConnection);
        });
        download->click();
        if (automatic) QVERIFY(!window.findChild<QFileDialog *>("sessionFileSaveDialog"));
        if (mode == 3) {
            QVERIFY(dialog->isVisible()); QVERIFY(download->isEnabled());
            QVERIFY(!QFile::exists(fixture + ".calls")); QVERIFY(!QFile::exists(destination)); return;
        }
        QVERIFY(!download->isEnabled()); QVERIFY(dialog->findChild<QProgressBar *>("sessionFileProgress")->isVisible());
        if (mode == 4) { dialog->reject(); return; }
        // Navigation cannot rebind the file transfer to a different machine.
        window.showSession({}, "codex/hgs/dashboard");
        QTRY_COMPARE(downloaded.size() + failed.size(), 1);
        QVERIFY(opened.urls.isEmpty());
        if (mode == 2 || mode == 7) {
            QCOMPARE(failed.size(), 1); QVERIFY(dialog->isVisible()); QVERIFY(download->isEnabled());
            QVERIFY(dialog->findChild<QLabel *>("sessionFileHint")->text().contains("Permission denied"));
            if (mode == 2) { QFile file(destination); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("previous copy")); }
            else QVERIFY(!QFileInfo::exists(destination));
            return;
        }
        QCOMPARE(downloaded.size(), 1);
        QFile file(destination); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("synthetic image"));
        if (mode == 0) {
            QVERIFY(dialog->isVisible());
            QVERIFY(dialog->findChild<QLabel *>("sessionFileHint")->text().contains(destination));
            dialog->findChild<QPushButton *>("openDownloadedSessionFile")->click();
        }
        QVERIFY(!dialog->isVisible());
    });
    window.findChild<ActivityView *>("mainActivity")->fileReferenceActivated(QString::fromUtf8(QUrl::toPercentEncoding(path, "/")));
    QVERIFY(dialog.isNull());
    if (mode == 0 || (automatic && mode != 7)) {
        QTRY_COMPARE(downloaded.size(), 1);
        if (mode == 5) { QTRY_COMPARE(opened.revealed, QStringList{destination}); QVERIFY(opened.urls.isEmpty()); }
        else { QTRY_COMPARE(opened.urls.size(), 1); QCOMPARE(opened.urls.first(), QUrl::fromLocalFile(destination)); }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        // Every completed download carries the system's downloaded-file marker.
        QVERIFY(!ExternalFile::downloadMarker(destination).isEmpty());
#endif
        QFile calls(fixture + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly));
        const auto args = QJsonDocument::fromJson(calls.readAll().trimmed()).array();
        QCOMPARE(args.size(), 6); auto prefix = args; prefix.removeLast();
        QCOMPARE(prefix, (QJsonArray{"@mac", "download", "--path", path, "--output"}));
        if (automatic) {
            const auto staging = args[5].toString();
            QCOMPARE(QFileInfo(staging).absolutePath(), cache); QVERIFY(QFileInfo(staging).fileName().startsWith(".zerus-part-"));
            QVERIFY(!QFileInfo::exists(staging));
        } else QCOMPARE(args[5].toString(), destination);
        if (mode == 6) { QFile existing(QDir(cache).filePath(QFileInfo(path).fileName())); QVERIFY(existing.open(QIODevice::ReadOnly)); QCOMPARE(existing.readAll(), QByteArray("existing copy")); }
    } else { QVERIFY(opened.urls.isEmpty()); }
}

void TestSessionsWindow::activityAttachmentsOpenLikeDownloads()
{
    // A conversation attachment opens quietly; a program is shown in its folder.
    QStandardPaths::setTestModeEnabled(true); const auto realPaths = qScopeGuard([] { QStandardPaths::setTestModeEnabled(false); });
    FolderUrlRecorder opened;
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *activity = window.findChild<ActivityView *>("mainActivity"); auto *notice = window.findChild<QLabel *>("notice");
    const auto document = AttachmentFiles::store("notes.txt", "attachment notes"), program = AttachmentFiles::store("setup.exe", "MZ fixture");
    const auto cleanup = qScopeGuard([&] { QFile::remove(document); QFile::remove(program); });
    QVERIFY(!document.isEmpty()); QVERIFY(!program.isEmpty());
    activity->attachmentActivated({{"name", "notes.txt"}, {"local_path", document}});
    QTRY_COMPARE(opened.urls.size(), 1); QCOMPARE(opened.urls.first().toLocalFile(), QFileInfo(document).canonicalFilePath());
    QTest::qWait(20); QVERIFY(!notice->text().contains("Operation failed")); QVERIFY(opened.revealed.isEmpty());
    activity->attachmentActivated({{"name", "setup.exe"}, {"local_path", program}});
    QTRY_COMPARE(opened.revealed.size(), 1); QCOMPARE(opened.urls.size(), 1);
    QTRY_VERIFY(notice->text().contains("setup.exe")); QVERIFY(!notice->property("error").toBool());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QVERIFY(!ExternalFile::downloadMarker(program).isEmpty());
#endif
}

void TestSessionsWindow::fileReferenceOpeningOutlivesDialog_data()
{
    QTest::addColumn<bool>("folder");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("platform");
    QTest::newRow("file") << false << "sample image.png" << "linux";
    QTest::newRow("folder") << true << "sample image.png" << "linux";
    // An agent's script is shown in its folder rather than run.
    QTest::newRow("program") << false << "deploy.sh" << "linux";
    // A Windows computer shows and copies backslashes, yet opens the Qt path.
    QTest::newRow("windows-file") << false << "sample image.png" << "windows";
    QTest::newRow("windows-folder") << true << "sample image.png" << "windows";
    // Before the first poll the platform is the GUI's own.
    QTest::newRow("unreported-platform") << false << "sample image.png" << "";
}

void TestSessionsWindow::fileReferenceOpeningOutlivesDialog()
{
    QFETCH(bool, folder); QFETCH(QString, name); QFETCH(QString, platform);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(name);
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("synthetic file"); file.close();
    const auto shown = platform == "windows" ? QString(path).replace('/', '\\')
        : platform.isEmpty() ? QDir::toNativeSeparators(path) : path;
    FolderUrlRecorder opened;
    auto state = fleet(); auto local = state.local();
    if (platform.isEmpty()) local.metrics.remove("platform"); else local.metrics["platform"] = platform;
    state.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); QTest::qWait(80);
    auto *activity = window.findChild<ActivityView *>("mainActivity");
    QSignalSpy copied(&window, &SessionsWindow::copyTextRequested);
    QPointer<QDialog> dialog;
    QTimer::singleShot(0, &window, [&] {
        dialog = window.findChild<QDialog *>("sessionFileDialog"); QVERIFY(dialog);
        const auto close = qScopeGuard([&] { if (dialog && dialog->isVisible()) dialog->reject(); });
        QCOMPARE(dialog->findChild<QLineEdit *>("sessionFilePath")->text(), shown);
        dialog->findChild<QPushButton *>("copySessionFilePath")->click();
        QVERIFY(dialog->isVisible());
        QCOMPARE(copied.size(), 1); QCOMPARE(copied.first().at(0).toString(), shown);
        QVERIFY(opened.urls.isEmpty());
        auto *button = dialog->findChild<QPushButton *>(folder ? "openSessionFileFolder" : "openSessionFile");
        QVERIFY(button); button->click();
        QVERIFY(!dialog->isVisible());
        // The desktop request must not belong to the disappearing modal window.
        QVERIFY(opened.urls.isEmpty());
    });
    activity->fileReferenceActivated(path);
    QVERIFY(dialog.isNull());
    QVERIFY(opened.urls.isEmpty()); QVERIFY(opened.revealed.isEmpty());
    if (name.endsWith(".sh")) {
        QTRY_COMPARE(opened.revealed, QStringList{path}); QVERIFY(opened.urls.isEmpty());
        QTRY_VERIFY(window.findChild<QLabel *>("notice")->text().contains("deploy.sh")); return;
    }
    QTRY_COMPARE(opened.urls.size(), 1);
    QCOMPARE(opened.urls.first(), QUrl::fromLocalFile(folder ? directory.path() : path));
    if (platform != "windows") return;
    // A peer's paths stay POSIX whatever this computer is.
    window.showSession("mac", "claude/infra/review"); QTest::qWait(50);
    QTimer::singleShot(0, &window, [&] {
        auto *remote = window.findChild<QDialog *>("sessionFileDialog"); QVERIFY(remote);
        const auto close = qScopeGuard([&] { remote->reject(); });
        QCOMPARE(remote->findChild<QLineEdit *>("sessionFilePath")->text(), QString("/Users/remote/docs/README.md"));
        remote->findChild<QPushButton *>("copySessionFilePath")->click();
    });
    activity->fileReferenceActivated("/Users/remote/docs/README.md");
    QCOMPARE(copied.size(), 2); QCOMPARE(copied.last().at(0).toString(), QString("/Users/remote/docs/README.md"));
}

void TestSessionsWindow::terminalDropQuotesPathsForTheMachine()
{
    // The local Windows machine reads native paths in double quotes, or in single
    // quotes when PowerShell would expand `$` or a backtick; a peer's staged copies and
    // a POSIX local machine keep single quotes.
    const auto program=m_dir->filePath("terminal-quoting-hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport json,os,runpy,sys\na=sys.argv[1:]\nif a and a[0].startswith('@'):a=a[1:]\n"
        "if a[:1]==['a']:\n if os.name!='nt':\n  import tty;tty.setraw(0)\n"
        " sys.stdout.write('\\x1b[?2004h');sys.stdout.flush();sys.stdin.buffer.read();sys.exit(0)\n"
        "if a[:1]==['attachment']:\n p=json.load(sys.stdin)\n"
        " print(json.dumps(dict(request_id=p['request_id'],name=a[1],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],"
        "status='staged',files=[dict(path=\"/srv/inbox/report's draft.pdf\") for _ in p['attachments']])));sys.exit(0)\n"
        "sys.argv[0]="+FakeCli::pythonString(script())+";runpy.run_path(sys.argv[0],run_name='__main__')\n");
    fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QStringList paths;
    for(const auto &name:{QString("plain.txt"),QString("report's draft.pdf"),QString("a&b.txt"),QString("cost $5's `q`.txt")}) {
        QFile file(m_dir->filePath(name));QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");paths<<file.fileName();
    }
    QMimeData mime;QList<QUrl> urls;for(const auto &path:paths)urls<<QUrl::fromLocalFile(path);mime.setUrls(urls);
    const auto native=[](QString path){return path.replace('/','\\');};
    const auto drop=[&](SessionsWindow &window,const QString &host,const QString &name) {
        window.showSession(host,name);
        auto *terminal=window.findChild<TerminalView *>();auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");
        tabs->setCurrentWidget(terminal);if(!QTest::qWaitFor([&]{return terminal->isConnected();},10000))return QByteArray();QTest::qWait(150);
        QSignalSpy output(terminal->terminalScreen(),&TerminalScreen::output);
        auto *panel=window.findChild<QWidget *>("sessionDetailPanel");
        QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&enter);
        QDropEvent event(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&event);
        if(!event.isAccepted())return QByteArray();
        QByteArray pasted;
        (void)QTest::qWaitFor([&]{pasted.clear();for(const auto &item:output)pasted+=item.first().toByteArray();return pasted.contains(".txt")||pasted.contains(".pdf");},20000);
        return pasted;
    };
    for(const auto &platform:{QString("windows"),QString("linux")}) {
        auto state=fleet();auto local=state.local();local.metrics["platform"]=platform;
        state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        auto mac=*state.peer("mac");mac.sessions[0].runId="run-mac";mac.sessions[0].conversationId="conversation-mac";
        state.setPeer(mac,QDateTime::currentMSecsSinceEpoch());
        SessionsWindow window(FakeCli::program(program));window.setFleet(state);window.resize(1100,760);window.show();
        const auto pasted=QString::fromUtf8(drop(window,{},"codex/hgs/dashboard"));
        if(platform=="windows") {
            QVERIFY2(pasted.contains(native(paths[0])+" \""+native(paths[1])+"\" \""+native(paths[2])+"\" '"
                                     +native(paths[3]).replace("'","''")+"' "),qPrintable(pasted));
            QVERIFY(!pasted.contains('/'));QVERIFY(!pasted.contains('\''+native(paths[1])));
        } else {
            QVERIFY2(pasted.contains('\''+paths[0]+"' '"+QString(paths[1]).replace("'","'\\''")+"' '"+paths[2]+"' '"
                                     +QString(paths[3]).replace("'","'\\''")+"' "),qPrintable(pasted));
            QVERIFY(!pasted.contains('"'));
        }
        // A peer's staged copies keep POSIX quoting whatever this computer runs.
        const auto remote=QString::fromUtf8(drop(window,"mac","claude/infra/review"));
        QVERIFY2(remote.contains("'/srv/inbox/report'\\''s draft.pdf' '/srv/inbox/report'\\''s draft.pdf' '/srv/inbox/report'\\''s draft.pdf' "),
                 qPrintable(remote));
        QVERIFY(!remote.contains('"'));QVERIFY(!remote.contains('\\'+QString("srv")));
    }
}

void TestSessionsWindow::terminalDropStagesIntoAnUntrackedRemoteShell()
{
    // A shell from `hgs sh -d` never gets a run identity: its listed creation time
    // binds the copy instead. A tracked session still waits for its identity.
    const auto program=m_dir->filePath("terminal-untracked-hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport json,os,runpy,sys\na=sys.argv[1:]\nif a and a[0].startswith('@'):a=a[1:]\n"
        "if a[:1]==['a']:\n if os.name!='nt':\n  import tty;tty.setraw(0)\n"
        " sys.stdout.write('\\x1b[?2004h');sys.stdout.flush();sys.stdin.buffer.read();sys.exit(0)\n"
        "if a[:1]==['inspect'] and a[1].startswith('sh/'):print(json.dumps(dict(name=a[1],tracked=False,events=[],cursor=0)));sys.exit(0)\n"
        "if a[:1]==['attachment']:\n p=json.load(sys.stdin);p.pop('attachments')\n"
        " open(__file__+'.stage','a').write(json.dumps(dict(args=sys.argv[1:],request=p))+'\\n')\n"
        " if os.path.exists(__file__+'.old'):print(a[1]+': no saved binding; restart through hgs to enable tracking',file=sys.stderr);sys.exit(1)\n"
        " created=p.get('expected_created',0)+os.path.exists(__file__+'.skew')\n"
        " print(json.dumps(dict(request_id=p['request_id'],name=a[1],run_id=p['expected_run_id'],conversation_id=p['expected_conversation_id'],"
        "created=created,status='staged',files=[dict(path='/srv/inbox/notes.txt')])));sys.exit(0)\n"
        "sys.argv[0]="+FakeCli::pythonString(script())+";runpy.run_path(sys.argv[0],run_name='__main__')\n");
    fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QFile file(m_dir->filePath("notes.txt"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
    QMimeData mime;mime.setUrls({QUrl::fromLocalFile(file.fileName())});
    auto state=fleet();auto mac=*state.peer("mac");
    SessionInfo shell;shell.name="sh/scratch";shell.cmd="sh";shell.project="scratch";shell.created=1700000000;mac.sessions<<shell;
    state.setPeer(mac,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(program));window.setFleet(state);window.resize(1100,760);window.show();
    auto *terminal=window.findChild<TerminalView *>();auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");
    auto *panel=window.findChild<QWidget *>("sessionDetailPanel");auto *notice=window.findChild<QLabel *>("notice");
    const auto stages=[&] {
        QList<QJsonObject> result;QFile log(program+".stage");
        if(log.open(QIODevice::ReadOnly))for(const auto &line:log.readAll().split('\n'))if(!line.isEmpty())result<<QJsonDocument::fromJson(line).object();
        return result;
    };
    const auto drop=[&](const QString &name) {
        window.showSession("mac",name);tabs->setCurrentWidget(terminal);
        if(!QTest::qWaitFor([&]{return terminal->isConnected();},10000))return QByteArray();QTest::qWait(150);
        QSignalSpy output(terminal->terminalScreen(),&TerminalScreen::output);const auto before=stages().size();
        QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&enter);
        QDropEvent event(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&event);
        if(!event.isAccepted())return QByteArray();
        QByteArray pasted;
        // A transfer ends in a pasted path or a notice other than its progress.
        (void)QTest::qWaitFor([&]{
            pasted.clear();for(const auto &item:output)pasted+=item.first().toByteArray();
            return pasted.contains("notes.txt")||(stages().size()>before&&!notice->text().startsWith("Transferring"));
        },20000);
        return pasted;
    };
    const auto pasted=drop("sh/scratch");
    QVERIFY2(pasted.contains("'/srv/inbox/notes.txt' "),pasted.constData());
    auto staged=stages();QCOMPARE(staged.size(),1);
    QCOMPARE(staged[0]["args"].toArray(),QJsonArray({"@mac","attachment","sh/scratch","--stage","--json"}));
    const auto request=staged[0]["request"].toObject();
    QCOMPARE(request["expected_run_id"].toString(),QString());QCOMPARE(request["expected_conversation_id"].toString(),QString());
    QVERIFY(request["expected_run_id"].isString());QCOMPARE(request["expected_created"].toInteger(),qint64(1700000000));

    // An acknowledgement for another creation time pastes nothing.
    QVERIFY(QFile(program+".skew").open(QIODevice::WriteOnly));
    QVERIFY(!drop("sh/scratch").contains("notes.txt"));
    QCOMPARE(notice->text(),QString("Invalid acknowledgement. Check Terminal before retrying."));QCOMPARE(stages().size(),2);
    QVERIFY(QFile::remove(program+".skew"));

    // An hgs that predates untracked drops asks for a saved binding.
    QVERIFY(QFile(program+".old").open(QIODevice::WriteOnly));
    QVERIFY(!drop("sh/scratch").contains("notes.txt"));
    QCOMPARE(notice->text(),QString("Update hgs on mac to drop files into an untracked session."));QCOMPARE(stages().size(),3);
    QVERIFY(QFile::remove(program+".old"));

    // A tracked session whose run identity has not loaded transfers nothing.
    window.showSession("mac","claude/infra/review");tabs->setCurrentWidget(terminal);
    QTRY_VERIFY_WITH_TIMEOUT(terminal->isConnected(),10000);QTest::qWait(150);
    QDragEnterEvent enter(QPoint(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&enter);
    QDropEvent event(QPointF(10,10),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(panel,&event);
    QCOMPARE(notice->text(),QString("Wait for the session identity to load, then drop the files again."));
    QTest::qWait(300);QCOMPARE(stages().size(),3);
}

void TestSessionsWindow::folderShellFollowsTheMachinePlatform()
{
    // The local Windows machine opens drive and UNC folders; a POSIX one, and
    // every peer, folders that start with '/'.
    for(const auto &platform:{QString("windows"),QString("linux")}) {
        const bool windows=platform=="windows";const auto now=QDateTime::currentMSecsSinceEpoch();
        auto state=fleet();auto local=state.local();local.metrics["platform"]=platform;
        local.sessions[0].cwd="C:\\work\\a folder";local.sessions[1].cwd="/work/docs";state.setLocal(local,now);
        auto mac=*state.peer("mac");mac.sessions[0].cwd="C:/work";state.setPeer(mac,now);
        SessionsWindow window(program());window.setFleet(state);window.show();
        auto *list=window.findChild<QListWidget *>("sessionList");auto *menu=window.findChild<QMenu *>("sessionContextMenu");
        auto *shell=window.findChild<QPushButton *>("openFolderShellAction");QVERIFY(shell);
        QSignalSpy requested(&window,&SessionsWindow::folderShellRequested);
        const auto menuAllows=[&](const QString &key) {
            for(int row=0;row<list->count();++row) if(list->item(row)->data(SessionRoles::Key).toString()==key) {
                const auto point=list->visualItemRect(list->item(row)).center();
                QContextMenuEvent event(QContextMenuEvent::Mouse,point,list->viewport()->mapToGlobal(point));
                QApplication::sendEvent(list->viewport(),&event);
                const bool enabled=menu->findChild<QAction *>("contextFolderShell")->isEnabled();menu->hide();return enabled;
            }
            return false;
        };
        window.showSession({},local.sessions[0].name);
        QCOMPARE(shell->isEnabled(),windows);QCOMPARE(menuAllows("\n"+local.sessions[0].name),windows);
        shell->click();
        if(windows){QCOMPARE(requested.size(),1);QCOMPARE(requested[0][0].toString(),QString());QCOMPARE(requested[0][1].toString(),QString("C:\\work\\a folder"));}
        else QCOMPARE(requested.size(),0);
        window.showSession({},local.sessions[1].name);
        QCOMPARE(shell->isEnabled(),!windows);QCOMPARE(menuAllows("\n"+local.sessions[1].name),!windows);
        window.showSession("mac",mac.sessions[0].name);
        QVERIFY(!shell->isEnabled());QVERIFY(!menuAllows("mac\n"+mac.sessions[0].name));
    }
}

void TestSessionsWindow::nativeSessionUsageStaysWithSelectedConversation()
{
    SessionsWindow window(program());window.setFleet(fleet());window.show();QTest::qWait(60);
    auto *client=window.findChild<HgsClient *>();auto *info=window.findChild<QTextBrowser *>("sessionInfo");
    const QJsonObject usage{{"status","ok"},{"totals",QJsonObject{{"total",12345},{"input",12000},{"cache_read",10000},{"output",345},{"reasoning",100}}},
        {"context",QJsonObject{{"used",1500},{"limit",10000}}}};
    client->inspectionReady({},"codex/hgs/dashboard",{{"conversation_id","conversation-one"},{"session_usage",usage}});
    const auto text=info->toPlainText().simplified();QVERIFY(text.contains("Session usage"));QVERIFY2(text.contains(QLocale().toString(12345).simplified()),qPrintable(text));
    QVERIFY(text.contains("Reasoning (in output)"));QVERIFY(window.findChild<QLabel *>("detailsContext")->text().contains("15"));QVERIFY(text.contains("Not reported"));
    auto *context=window.findChild<QPushButton *>("activityContext");QVERIFY(context->isVisible());
    QCOMPARE(context->property("contextPercent").toDouble(),15.);QVERIFY(context->toolTip().contains(QLocale().toString(1500)));
    const auto indicatorSize=context->size();context->click();QCOMPARE(window.findChild<QTabWidget *>("sessionInspector")->currentIndex(),1);
    window.findChild<QPushButton *>("toggleInspector")->setChecked(false);

    const auto preview=qEnvironmentVariable("HGS_SESSION_USAGE_PREVIEW");
    if(!preview.isEmpty()) {
        window.resize(1260,920);window.findChild<QPushButton *>("toggleInspector")->setChecked(true);
        window.findChild<QTabWidget *>("sessionInspector")->setCurrentIndex(1);QTest::qWait(40);QVERIFY(window.grab().save(preview));
    }
    window.showSession("mac","claude/infra/review");
    QVERIFY(!info->toPlainText().simplified().contains(QLocale().toString(12345).simplified()));
    client->inspectionReady("mac","claude/infra/review",{{"conversation_id","conversation-one"}});
    QVERIFY(info->toPlainText().contains("No native usage reported"));
    QVERIFY(!context->property("contextPercent").isValid());QCOMPARE(context->height(),indicatorSize.height());   // width follows the text
    QVERIFY(context->text().isEmpty());QVERIFY(!context->isEnabled());
    auto noCapacity=usage;noCapacity["context"]=QJsonObject{{"used",32000}};
    client->inspectionReady("mac","claude/infra/review",{{"conversation_id","conversation-one"},{"session_usage",noCapacity}});
    QVERIFY(context->text().contains("32"));QVERIFY(!context->property("contextPercent").isValid());
    QVERIFY(context->toolTip().contains("Percentage is unavailable"));
    QVERIFY(!context->text().contains('%')); QVERIFY(!context->text().contains('/'));
    QVERIFY(!context->text().contains(QChar(0x2014))); QVERIFY(!context->icon().isNull());
    QVERIFY(context->accessibleName().contains("capacity unknown")); QVERIFY(context->isEnabled());
    client->inspectionReady("mac","claude/infra/review",{{"conversation_id","conversation-one"},{"session_usage",usage}});
    QVERIFY(context->text().contains('%')); QVERIFY(context->icon().isNull());
}

void TestSessionsWindow::compactMetadataAndSubagentRoster()
{
    QSettings().setValue("workspace/showCompletedSubagents", true);
    auto state = fleet(); auto box = state.local(); auto &session = box.sessions[0];
    session.gitBranch = "feat/compact"; session.gitWorktree = true; session.gitWorktreeName = "ui-tree"; session.gitCommonDir = "/work/ui/.git";
    session.cwd = "/work/ui-tree"; session.gitRoot = session.cwd; session.activitySummary = "Read"; session.activityDetail = "view.cpp";
    session.subagentSource = "hooks"; session.subagentCountsComplete = true; session.subagentActiveCount = 2; session.subagentTotalCount = 14;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); QTest::qWait(60);
    auto *list = window.findChild<QListWidget *>("sessionList"); QVERIFY(list->sizeHintForRow(0) <= 96);
    QCOMPARE(list->item(0)->data(Qt::UserRole + 1).toString(), QString("dashboard"));
    QCOMPARE(list->item(0)->data(Qt::UserRole + 7).toString(), QString("2/14"));
    // A linked worktree names its main checkout folder, not its own generated folder.
    QCOMPARE(list->item(0)->data(SessionRoles::Meta).toString(), QString("ui / feat/compact"));
    QCOMPARE(list->item(0)->data(SessionRoles::MetaIcon).toString(), QString("git-branch"));
    const auto accessible = list->item(0)->data(Qt::AccessibleTextRole).toString();
    QVERIFY(accessible.contains("ui / feat/compact")); QVERIFY(!accessible.contains("worktree: ui-tree")); QVERIFY(accessible.contains("/work/ui-tree"));
    QVERIFY(accessible.contains("Read: view.cpp")); QVERIFY(accessible.contains("2 active / 14 total"));
    auto *header = window.findChild<QLabel *>("sessionMeta");
    QCOMPARE(header->accessibleName(), QString("ui / feat/compact · worktree"));
    QVERIFY(header->toolTip().contains("/work/ui-tree")); QVERIFY(!header->toolTip().contains("ui-tree / "));
    const auto info = window.findChild<QTextBrowser *>("sessionInfo")->toPlainText();
    QVERIFY(info.contains("ui / feat/compact · worktree")); QVERIFY(info.contains("/work/ui-tree")); QVERIFY(!info.contains("worktree: ui-tree"));
    auto *search = window.findChild<QLineEdit *>("search"); search->setText("feat/compact"); QCOMPARE(window.findChild<QListWidget *>("searchResults")->count(), 1); search->clear();
    QJsonObject children;
    for (int i = 0; i < 14; ++i) children[QString("agent-%1").arg(i)] = QJsonObject{{"label", QString("Child %1").arg(i)}, {"state", i < 2 ? "working" : "finished"}, {"detail", "Review code"}};
    children["unknown"] = QJsonObject{{"label", "Uncertain"}, {"state", "working"}, {"display_state", "unknown"}};
    QJsonObject details{{"tracked", true}, {"conversation_id", "conversation-one"}, {"subagent_source", "hooks"}, {"subagents", children},
        {"subagent_active_count", 2}, {"subagent_total_count", 15}, {"subagent_counts_complete", false}};
    auto *client = window.findChild<HgsClient *>(); client->inspectionReady({}, session.name, details);
    auto *roster = window.findChild<QTextBrowser *>("subagents"); const auto text = roster->toPlainText();
    QVERIFY(text.contains("Child 13")); QVERIFY(text.contains("Ready")); QVERIFY(text.contains("Uncertain")); QVERIFY(text.contains("Unknown"));
    QVERIFY(!text.contains("completed tasks"));
    QVERIFY(text.contains("some states unknown"));
    bool hasFullTooltip = false;
    for (auto *frame : roster->document()->rootFrame()->childFrames()) if (auto *table = qobject_cast<QTextTable *>(frame)) {
        auto cursor = table->cellAt(0, 0).firstCursorPosition(); cursor.movePosition(QTextCursor::NextCharacter);
        hasFullTooltip = cursor.charFormat().toolTip().contains("Review code");
    }
    QVERIFY(!hasFullTooltip);
    QVERIFY(roster->toHtml().contains("hgs-agent:"));
    // A cached busy hook on a stopped terminal must not imply current activity.
    box.sessions[0].processState = "exited"; state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(list->item(0)->data(Qt::UserRole + 7).toString().isEmpty());
    QCOMPARE(list->item(0)->data(Qt::UserRole + 4).toString(), QString("Agent exited"));
    for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "active") QCOMPARE(button->property("count").toInt(), 1);
    client->inspectionReady({}, session.name, details); QVERIFY(roster->toPlainText().contains("Last recorded"));
    QVERIFY(!roster->toPlainText().contains("2 active /"));
}

void TestSessionsWindow::subagentActivityKeepsMainDraftAndRejectsStaleHistory()
{
    QSettings().setValue("workspace/showCompletedSubagents", true);
    QJsonObject details{{"tracked",true},{"conversation_id","conversation-one"},{"run_id","run-one"},
        {"subagents",QJsonObject{{"child-1",QJsonObject{{"name","Review"},{"state","finished"},{"detail",QString(10000,'x')}}}}}};
    details["session_usage"]=QJsonObject{{"context",QJsonObject{{"used",8000},{"limit",10000}}}};
    QTemporaryDir fixtureDir; const auto program=fixtureDir.filePath("hgs");
    QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(__file__).parent
if 'inspect' in sys.argv: print((root/('child.json' if '--agent' in sys.argv else 'parent.json')).read_text())
else: print('{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QJsonObject child{{"parent_conversation_id","conversation-one"},{"conversation_id","conversation-one/child-1"},{"run_id","run-one"},
        {"history_scope","Native subagent history"},{"events",QJsonArray{QJsonObject{{"type","AgentMessage"},{"detail","Review finished"},{"at",1700000000}}}}};
    child["session_usage"]=QJsonObject{{"context",QJsonObject{{"used",500},{"limit",2000}}}};
    for(const auto &snapshot: {qMakePair(QString("parent.json"),details),qMakePair(QString("child.json"),child)}) {
        QVERIFY(FakeCli::replace(fixtureDir.filePath(snapshot.first),QJsonDocument(snapshot.second).toJson()));
    }
    SessionsWindow window(FakeCli::program(program));window.setFleet(fleet());window.show();
    auto *client=window.findChild<HgsClient *>();QSignalSpy inspected(client,&HgsClient::inspectionReady);QTRY_VERIFY(!inspected.isEmpty());
    auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *editor=composer->editor();setComposerText(composer,"Keep my main draft");
    client->inspectionReady({},"codex/hgs/dashboard",details);
    auto *roster = window.findChild<QTextBrowser *>("subagents");
    auto *stack = window.findChild<QStackedWidget *>("activityStack");
    roster->anchorClicked(QUrl("hgs-agent:unknown")); QCOMPARE(stack->currentIndex(),0);
    QSignalSpy childInspected(client,&HgsClient::subagentInspectionReady);
    roster->anchorClicked(QUrl("hgs-agent:child-1")); QCOMPARE(stack->currentIndex(),1); QVERIFY(!composer->isVisible());
    QVERIFY(!window.findChild<QLabel *>("subagentId"));
    QVERIFY(window.findChild<QTextBrowser *>("sessionInfo")->toPlainText().contains("child-1"));
    auto *view = window.findChild<ActivityView *>("subagentActivity");
    QTRY_VERIFY(!childInspected.isEmpty());
    client->subagentInspectionReady({},"codex/hgs/dashboard","child-1",{},child);
    QCOMPARE(window.findChild<QLabel *>("detailsIdentityValue")->text(), QString("conversation-one/child-1"));
    QSignalSpy copied(&window, &SessionsWindow::copyTextRequested);
    window.findChild<QPushButton *>("copySessionIdAction")->click(); QCOMPARE(copied.size(), 1);
    QCOMPARE(copied[0][0].toString(), QString("conversation-one/child-1"));
    window.findChild<QPushButton *>("headerCopySessionIdAction")->click(); QCOMPARE(copied.size(), 2);
    QCOMPARE(copied[1][0].toString(), QString("conversation-one/child-1"));
    QTRY_VERIFY2(view->browser()->toPlainText().contains("Review finished"),qPrintable(view->browser()->toPlainText()));
    QCOMPARE(window.findChild<QPushButton *>("subagentContext")->property("contextPercent").toDouble(),25.);
    auto *childComposer=window.findChild<MessageComposer *>("subagentComposer");auto *childContext=window.findChild<QPushButton *>("subagentContext");
    QVERIFY(childComposer->isVisible());QVERIFY(!childComposer->isInputVisible());QVERIFY(childContext->isVisible());
    QVERIFY(childComposer->toolbar()->isAncestorOf(childContext));
    QCOMPARE(window.findChild<QPushButton *>("activityContext")->property("contextPercent").toDouble(),80.);
    window.findChild<QPushButton *>("subagentBack")->click(); QCOMPARE(stack->currentIndex(),0);
    QCOMPARE(editor->toPlainText(),QString("Keep my main draft"));
    roster->anchorClicked(QUrl("hgs-agent:child-1"));
    window.showSession({},"kimi/docs/research"); QCOMPARE(stack->currentIndex(),0);
    client->subagentInspectionReady({},"codex/hgs/dashboard","child-1",{},child);
    QCOMPARE(stack->currentIndex(),0);
}

void TestSessionsWindow::childTreeKeepsRoutineResultsQuiet()
{
    QSettings().setValue("workspace/showCompletedSubagents", true);
    FleetState state; auto box = fleet().local(); auto session = box.sessions[0];
    session.conversationId = "tree-conversation"; session.runId = "tree-run";
    session.activity = "idle"; session.phase = "idle"; session.currentTool.clear();
    session.subagentCountsComplete = true; session.subagentTotalCount = 2; session.subagentActiveCount = 1;
    session.subagents = {{"agent-a", QJsonObject{{"name","Review"},{"state","working"},{"detail","Checking changes"}}},
        {"agent-b", QJsonObject{{"name","Tests"},{"state","finished"},{"detail","All tests pass"},{"reply_id","first-result"}}}};
    box.sessions = {session};
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); QTest::qWait(60);
    auto *list = window.findChild<SessionList *>("sessionList"); QVERIFY(list);
    auto *parent = list->item(0); QVERIFY(parent->data(SessionRoles::HasChildren).toBool());
    QVERIFY(!parent->data(SessionRoles::Unread).toBool());
    QVERIFY(!parent->data(SessionRoles::Attention).toBool());
    auto *brand = window.findChild<QPushButton *>("brandMark");
    auto *badge = window.findChild<QLabel *>("railAttentionBadge");
    QCOMPARE(brand->property("attentionCount").toInt(), 0); QVERIFY(!badge->isVisible());
    const auto rootKey = parent->data(SessionRoles::Key).toString();
    const auto count = list->count();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(parent).topLeft() + QPoint(16, 21));
    QCOMPARE(list->count(), count); // The title selects the root; only the trailing counter expands it.
    const auto togglePoint = [&] { return list->visualItemRect(list->item(0)).bottomRight() - QPoint(20, 18); };
    QTest::mouseMove(list->viewport(), togglePoint());
    QCOMPARE(list->viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, togglePoint());
    QCOMPARE(list->count(), count + 2);
    QCOMPARE(list->item(1)->data(SessionRoles::ChildId).toString(), QString("agent-a"));
    QVERIFY(!list->item(1)->flags().testFlag(Qt::ItemIsDragEnabled));
    QVERIFY(!list->item(2)->data(SessionRoles::Unread).toBool());
    QVERIFY(list->item(2)->data(Qt::AccessibleTextRole).toString().contains("Ready"));
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Parent draft");
    list->setCurrentRow(1);
    QCOMPARE(window.findChild<QStackedWidget *>("activityStack")->currentIndex(), 1);
    QVERIFY(!composer->isVisible());
    QVERIFY(!window.findChild<QPushButton *>("more")->isVisible());
    window.setFleet(state);
    QCOMPARE(list->currentItem()->data(SessionRoles::ChildId).toString(), QString("agent-a"));
    QTest::keyClick(list, Qt::Key_Left);
    QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(), rootKey);
    QCOMPARE(composer->editor()->toPlainText(), QString("Parent draft"));
    QTest::keyClick(list, Qt::Key_Left); QCOMPARE(list->count(), count);
    list->setProperty("compact", false); list->doItemsLayout();
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, togglePoint());
    QCOMPARE(list->count(), count + 2);
    QTest::keyClick(list, Qt::Key_Left); QCOMPARE(list->count(), count);
    QAction *readAll = nullptr;
    for (auto *action : window.findChildren<QAction *>()) if (action->text().startsWith("Mark all as read")) readAll = action;
    QVERIFY(readAll); QVERIFY(!readAll->isEnabled());
    // New child results and routine failures remain quiet without opening them.
    for (const auto &status : {"finished", "ready", "idle", "error"}) {
        box.sessions[0].subagents["agent-b"] = QJsonObject{{"name","Tests"},{"state",status},{"detail","New internal result"},{"reply_id","second-result"}};
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
        QVERIFY(!list->item(0)->data(SessionRoles::Unread).toBool());
        QVERIFY(!list->item(0)->data(SessionRoles::Attention).toBool());
        QCOMPARE(brand->property("attentionCount").toInt(), 0); QVERIFY(!readAll->isEnabled());
    }
    auto *filter = [&]() -> QPushButton * { for (auto *button : window.findChildren<QPushButton *>())
        if (button->property("filter") == "attention") return button; return nullptr; }();
    QVERIFY(filter); filter->click(); QCOMPARE(list->count(), 0);
    // An explicit child approval still reaches the attention filter and rail.
    box.sessions[0].subagents["agent-b"] = QJsonObject{{"name","Tests"},{"state","working"},{"display_state","approval"}};
    box.sessions[0].subagentActiveCount = 2;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->count(), 1); QVERIFY(list->item(0)->data(SessionRoles::Attention).toBool());
    QCOMPARE(brand->property("attentionCount").toInt(), 1); QVERIFY(badge->isVisible()); QVERIFY(!readAll->isEnabled());
    box.sessions[0].subagents["agent-b"] = QJsonObject{{"name","Tests"},{"state","finished"}};
    box.sessions[0].subagentActiveCount = 1;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->count(), 0); QCOMPARE(brand->property("attentionCount").toInt(), 0);
    // An intermediate parent reply remains unread while work continues.
    box.sessions[0].activity = "busy"; box.sessions[0].phase = "tool";
    box.sessions[0].replyId = "12:120";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->count(), 0); QCOMPARE(brand->property("attentionCount").toInt(), 0);
    QVERIFY(state.local().sessions[0].unreadReply);
    QVERIFY(readAll->isEnabled()); // The exact unread reply is retained for explicit reading.
    // Finishing the parent turn still waits for its active child.
    box.sessions[0].activity = "idle"; box.sessions[0].phase = "idle";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->count(), 0); QCOMPARE(brand->property("attentionCount").toInt(), 0);
    // Finishing that child reveals the same unread reply without a new reply ID.
    box.sessions[0].subagents["agent-a"] = QJsonObject{{"name","Review"},{"state","finished"}};
    box.sessions[0].subagentActiveCount = 0;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->count(), 1); QVERIFY(list->item(0)->data(SessionRoles::Unread).toBool());
    QVERIFY(readAll->isEnabled()); QSignalSpy read(&window, &SessionsWindow::repliesMarkedRead);
    readAll->trigger(); QCOMPARE(read.size(), 1); QCOMPARE(read[0][0].toJsonObject().size(), 1);
    QVERIFY(!list->item(0)->data(SessionRoles::Unread).toBool()); QVERIFY(!readAll->isEnabled());
}

void TestSessionsWindow::completedSubagentsPreferenceKeepsOpenHistory()
{
    FleetState state; auto box = fleet().local(); auto session = box.sessions[0];
    session.conversationId = "conversation-one"; session.runId = "run-one";
    session.subagentSource = "hooks"; session.subagentCountsComplete = true;
    session.subagentActiveCount = 1; session.subagentTotalCount = 29;
    session.subagents = {{"a", QJsonObject{{"name", "Open child"}, {"state", "working"}}},
        {"b", QJsonObject{{"name", "Past child"}, {"state", "finished"}}},
        {"c", QJsonObject{{"name", "Approval child"}, {"state", "finished"}, {"display_state", "approval"}}},
        {"d", QJsonObject{{"name", "Unknown child"}}}};
    box.sessions = {session}; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QJsonObject details{{"tracked", true}, {"conversation_id", session.conversationId}, {"run_id", session.runId},
        {"subagent_source", "hooks"}, {"subagents", session.subagents}, {"subagent_counts_complete", true},
        {"subagent_active_count", 1}, {"subagent_total_count", 29}};
    QJsonArray events;
    for (int i = 0; i < 80; ++i) events.append(QJsonObject{{"seq", i+1}, {"type", "AgentMessage"}, {"detail", QString("Retained child history %1").arg(i)}, {"at", 1700000000+i}});
    const QJsonObject childDetails{{"parent_conversation_id", session.conversationId},
        {"conversation_id", "child-conversation"}, {"run_id", "run-one"}, {"send_supported", true}, {"events", events}};
    // Every asynchronous inspection sees the fixture's current roster, rather
    // than replacing an injected finished turn with the generic working fleet.
    const auto fixtureScript = m_dir->filePath("hgs-completed-subagents");
    const auto saveSnapshot = [&](const QString &suffix, const QJsonObject &snapshot) {
        QVERIFY(FakeCli::replace(fixtureScript + suffix, QJsonDocument(snapshot).toJson(QJsonDocument::Compact)));
    };
    saveSnapshot(".parent", details); saveSnapshot(".child", childDetails);
    QFile fixture(fixtureScript); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport sys\na=sys.argv[1:]\nif a and a[0].startswith('@'):a=a[1:]\n"
        "if a[:1]==['inspect']:sys.stdout.write(open(__file__+('.child' if '--agent' in a else '.parent')).read())\n");
    fixture.close(); QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    SessionsWindow window(FakeCli::program(fixtureScript)); auto *client = window.findChild<HgsClient *>();
    QSignalSpy parentInspected(client, &HgsClient::inspectionReady);
    QSignalSpy childInspected(client, &HgsClient::subagentInspectionReady);
    window.setFleet(state); window.show(); QTRY_VERIFY(!parentInspected.isEmpty());
    auto *list = window.findChild<SessionList *>("sessionList");
    auto *roster = window.findChild<QTextBrowser *>("subagents");
    auto *check = window.findChild<QCheckBox *>("workspaceShowCompletedSubagents"); QVERIFY(check); QVERIFY(!check->isChecked());
    const auto rootKey = list->item(0)->data(SessionRoles::Key).toString();
    list->childrenToggled(rootKey); QCOMPARE(list->count(), 4);
    QCOMPARE(list->item(1)->data(SessionRoles::ChildId).toString(), QString("a"));
    QCOMPARE(list->item(2)->data(SessionRoles::ChildId).toString(), QString("c"));
    QVERIFY(roster->toPlainText().contains("Needs input")); QVERIFY(roster->toPlainText().contains("Past child"));
    QCOMPARE(list->item(0)->data(SessionRoles::Children).toString(), QString("1"));
    check->setChecked(true); QCOMPARE(list->count(), 5); QVERIFY(roster->toPlainText().contains("Past child"));
    check->setChecked(false); QCOMPARE(list->count(), 4);
    auto *parentComposer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(parentComposer, "Parent draft");
    const auto beforeOpen = parentInspected.size(); list->setCurrentRow(1);
    QTRY_VERIFY(parentInspected.size() > beforeOpen); QTRY_VERIFY(!childInspected.isEmpty());
    auto *childComposer = window.findChild<MessageComposer *>("subagentComposer"); setComposerText(childComposer, "Child draft");
    auto *view = window.findChild<ActivityView *>("subagentActivity"); auto *scroll = view->browser()->verticalScrollBar();
    scroll->setValue(scroll->maximum()/2); const auto position = scroll->value();
    const auto history = view->browser()->toPlainText();
    auto children = details.value("subagents").toObject(); auto child = children.value("a").toObject(); child["state"] = "finished"; children["a"] = child;
    details["subagents"] = children; saveSnapshot(".parent", details); client->inspectionReady({}, session.name, details);
    check->setChecked(true); check->setChecked(false);
    QCOMPARE(list->currentItem()->data(SessionRoles::ChildId).toString(), QString("a"));
    QCOMPARE(childComposer->editor()->toPlainText(), QString("Child draft"));
    QCOMPARE(view->browser()->toPlainText(), history); QCOMPARE(scroll->value(), position);
    childComposer->editor()->clear(); window.findChild<QPushButton *>("subagentBack")->click();
    QTRY_COMPARE(list->count(), 3); QCOMPARE(parentComposer->editor()->toPlainText(), QString("Parent draft"));
    // The window may still be inspecting the parent it returned to (slowly under
    // load); a request for the same session starts once that one has finished.
    bool requested = false;
    QTRY_VERIFY_WITH_TIMEOUT(requested || (requested = client->requestInspection({}, session.name, 0)), 10000);
    const auto beforeReinspect = parentInspected.size(); QTRY_VERIFY(parentInspected.size() > beforeReinspect);
    QCOMPARE(list->count(), 3); QCOMPARE(parentComposer->editor()->toPlainText(), QString("Parent draft"));
    // The inspector retains completed previews independently of the list filter.
    details["subagents"] = QJsonObject{};
    details["subagent_previews"] = QJsonArray{QJsonObject{{"id", "preview"}, {"name", "Past preview"}, {"state", "finished"}}};
    saveSnapshot(".parent", details); client->inspectionReady({}, session.name, details);
    QVERIFY(roster->toPlainText().contains("Past preview"));
    QVERIFY(!roster->toPlainText().contains("Completed turns are hidden"));
    check->setChecked(true); QVERIFY(roster->toPlainText().contains("Past preview"));
    SessionsWindow reopened(program()); QVERIFY(reopened.findChild<QCheckBox *>("workspaceShowCompletedSubagents")->isChecked());
    // A saved old-conversation draft must not pin a reused ID in a new roster.
    check->setChecked(false); details["subagents"] = children; details.remove("subagent_previews");
    saveSnapshot(".parent", details); client->inspectionReady({}, session.name, details);
    roster->anchorClicked(QUrl("hgs-agent:a")); setComposerText(childComposer, "Old conversation draft");
    box.sessions[0].conversationId = "replacement-conversation"; box.sessions[0].subagents = {{"a", child}};
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    for (int row = 0; row < list->count(); ++row) QVERIFY(list->item(row)->data(SessionRoles::ChildId).toString() != "a");
}

void TestSessionsWindow::inactiveSubagentsStayInInspectorAndOutOfSessionCounts()
{
    FleetState state; auto box = fleet().local(); box.sessions = {box.sessions[0]};
    auto &session = box.sessions[0]; session.conversationId = "conversation-one"; session.runId = "run-one";
    session.subagentSource = "hooks"; session.subagentCountsComplete = true;
    session.subagentActiveCount = 0; session.subagentTotalCount = 175;
    for (int i = 0; i < 175; ++i)
        session.subagents[QString("child-%1").arg(i, 3, 10, QChar('0'))] = QJsonObject{
            {"name", QString("Completed child %1").arg(i)}, {"state", "finished"}};
    const auto program = m_dir->filePath("hgs-full-roster");
    QFile scriptFile(program); QVERIFY(scriptFile.open(QIODevice::WriteOnly));
    scriptFile.write("#!/usr/bin/env python3\nimport sys\nif sys.argv[1:2]==['inspect']:sys.stdout.write(open(__file__+'.parent').read())\n"); scriptFile.close();
    QVERIFY(FakeCli::makeRunnable(scriptFile.fileName()));
    const auto snapshot = [&] {
        return QJsonObject{{"tracked", true}, {"conversation_id", session.conversationId}, {"run_id", session.runId},
            {"subagent_source", session.subagentSource}, {"subagents", session.subagents},
            {"subagent_counts_complete", session.subagentCountsComplete},
            {"subagent_active_count", session.subagentActiveCount}, {"subagent_total_count", session.subagentTotalCount}};
    };
    const auto save = [&] {
        QVERIFY(FakeCli::replace(program + ".parent", QJsonDocument(snapshot()).toJson(QJsonDocument::Compact)));
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    };
    save(); SessionsWindow window(FakeCli::program(program)); auto *client = window.findChild<HgsClient *>();
    QSignalSpy inspected(client, &HgsClient::inspectionReady);
    window.setFleet(state); window.show(); QTRY_VERIFY(!inspected.isEmpty());
    auto *list = window.findChild<SessionList *>("sessionList"); auto *roster = window.findChild<QTextBrowser *>("subagents");
    auto *check = window.findChild<QCheckBox *>("workspaceShowCompletedSubagents");
    const auto key = list->item(0)->data(SessionRoles::Key).toString();
    QCOMPARE(list->count(), 1); QVERIFY(list->item(0)->data(SessionRoles::Children).toString().isEmpty());
    QVERIFY(!list->item(0)->data(SessionRoles::HasChildren).toBool());
    QVERIFY(SessionList::childrenControlRect(list->visualItemRect(list->item(0)), list->model()->index(0,0), list->font()).isEmpty());
    QVERIFY(!list->item(0)->data(Qt::AccessibleTextRole).toString().contains("175"));
    QVERIFY(roster->toPlainText().contains("0 active / 175 total"));
    for (int i = 0; i < 175; ++i) QVERIFY(roster->toPlainText().contains(QString("Completed child %1").arg(i)));
    auto child = session.subagents["child-000"].toObject(); child["state"] = "working"; session.subagents["child-000"] = child;
    session.subagentActiveCount = 1; save(); window.setFleet(state); client->inspectionReady({}, session.name, snapshot());
    QCOMPARE(list->item(0)->data(SessionRoles::Children).toString(), QString("1"));
    QVERIFY(list->item(0)->data(Qt::AccessibleTextRole).toString().contains("1 active"));
    list->childrenToggled(key); QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(1)->data(SessionRoles::ChildId).toString(), QString("child-000"));
    QVERIFY(roster->toPlainText().contains("1 active / 175 total"));
    check->setChecked(true); QCOMPARE(list->count(), 176);
    QCOMPARE(list->item(0)->data(SessionRoles::Children).toString(), QString("1/175"));
    check->setChecked(false); QCOMPARE(list->count(), 2);
    child["state"] = "finished"; session.subagents["child-000"] = child; session.subagentActiveCount = 0;
    save(); window.setFleet(state); client->inspectionReady({}, session.name, snapshot());
    QCOMPARE(list->count(), 1); QVERIFY(list->item(0)->data(SessionRoles::Children).toString().isEmpty());
    QVERIFY(!list->item(0)->data(SessionRoles::HasChildren).toBool());
    QVERIFY(roster->toPlainText().contains("Completed child 174"));
    // A failed turn has ended too: the inspector keeps its error, the tree hides it.
    child["display_state"] = "error"; child["detail"] = "Usage limit reached"; session.subagents["child-000"] = child;
    save(); window.setFleet(state); client->inspectionReady({}, session.name, snapshot());
    QCOMPARE(list->count(), 1); QVERIFY(!list->item(0)->data(SessionRoles::HasChildren).toBool());
    QVERIFY(roster->toPlainText().contains("Error")); QVERIFY(roster->toPlainText().contains("Usage limit reached"));
    check->setChecked(true); QCOMPARE(list->count(), 176);
    check->setChecked(false); QCOMPARE(list->count(), 1);
    // Unknown evidence remains explicit; an incomplete hook roster cannot claim zero active.
    session.subagentCountsComplete = false; save(); window.setFleet(state); client->inspectionReady({}, session.name, snapshot());
    QCOMPARE(list->item(0)->data(SessionRoles::Children).toString(), QString("0+"));
    QVERIFY(roster->toPlainText().contains("some states unknown"));
}

void TestSessionsWindow::openCodeChildrenStayReadOnlyWithUnknownCounts()
{
    // `hgs inspect` shape for an OpenCode parent: one classic child finished,
    // and one whose turn end the API host has not observed.
    FleetState state; auto box = fleet().local(); box.sessions = {box.sessions[1]};
    auto &session = box.sessions[0]; session.name = "opencode/docs/review"; session.cmd = "opencode";
    session.project = "docs"; session.tag = "review"; session.conversationId = "ses_parent"; session.runId = "run-one";
    session.subagentSource = "opencode_native"; session.subagentCountsComplete = false;
    session.subagentActiveCount = 0; session.subagentCompletedCount = 1; session.subagentTotalCount = 2;
    session.subagents = {{"ses_done", QJsonObject{{"name", "Finished review (@general subagent)"}, {"state", "finished"}, {"detail", "Native child turn completed"}}},
        {"ses_open", QJsonObject{{"name", "Open review (@general subagent)"}, {"state", "unknown"}, {"detail", "Turn end not recorded"}}}};
    box.sessions = {session}; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    const QJsonObject details{{"tracked", true}, {"backend", "opencode"}, {"conversation_id", session.conversationId}, {"run_id", session.runId},
        {"subagent_source", "opencode_native"}, {"subagents", session.subagents}, {"subagent_counts_complete", false},
        {"subagent_active_count", 0}, {"subagent_completed_count", 1}, {"subagent_unknown_count", 1}, {"subagent_total_count", 2}};
    const QJsonObject childDetails{{"name", session.name}, {"parent_conversation_id", session.conversationId}, {"agent_id", "ses_open"},
        {"conversation_id", "ses_parent/ses_open"}, {"run_id", session.runId}, {"send_supported", false}, {"read_only", true},
        {"history_scope", "Native OpenCode subagent history"}, {"phase", "unknown"},
        {"events", QJsonArray{QJsonObject{{"seq", 1}, {"type", "UserPromptSubmit"}, {"detail", "Review the synthetic docs"}, {"at", 1700000000}}}}};
    const auto fixtureScript = m_dir->filePath("hgs-opencode-children");
    QVERIFY(FakeCli::replace(fixtureScript + ".parent", QJsonDocument(details).toJson(QJsonDocument::Compact)));
    QVERIFY(FakeCli::replace(fixtureScript + ".child", QJsonDocument(childDetails).toJson(QJsonDocument::Compact)));
    QFile fixture(fixtureScript); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("#!/usr/bin/env python3\nimport sys\na=sys.argv[1:]\nif a and a[0].startswith('@'):a=a[1:]\n"
        "if a[:1]==['inspect']:sys.stdout.write(open(__file__+('.child' if '--agent' in a else '.parent')).read())\n");
    fixture.close(); QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    SessionsWindow window(FakeCli::program(fixtureScript)); auto *client = window.findChild<HgsClient *>();
    QSignalSpy parentInspected(client, &HgsClient::inspectionReady);
    QSignalSpy childInspected(client, &HgsClient::subagentInspectionReady);
    window.setFleet(state); window.show(); QTRY_VERIFY(!parentInspected.isEmpty());
    auto *list = window.findChild<SessionList *>("sessionList"); auto *roster = window.findChild<QTextBrowser *>("subagents");
    // No confirmed active child, and the unknown one keeps the uncertainty marker.
    QCOMPARE(list->item(0)->data(SessionRoles::Children).toString(), QString("0+"));
    QVERIFY(roster->toPlainText().contains("0 confirmed active / 2 observed; some states unknown"));
    QVERIFY(roster->toPlainText().contains("Turn end not recorded"));
    list->childrenToggled(list->item(0)->data(SessionRoles::Key).toString()); QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(1)->data(SessionRoles::ChildId).toString(), QString("ses_open"));
    list->setCurrentRow(1); QTRY_VERIFY(!childInspected.isEmpty());
    auto *childComposer = window.findChild<MessageComposer *>("subagentComposer");
    QTRY_VERIFY(!childComposer->isInputVisible());
    bool readOnly = false;
    for (const auto *label : window.findChildren<QLabel *>())
        readOnly |= label->text().startsWith("Native OpenCode subagent history") && label->text().contains("Read only.");
    QVERIFY(readOnly);
    QTRY_VERIFY(window.findChild<ActivityView *>("subagentActivity")->browser()->toPlainText().contains("Review the synthetic docs"));
}

namespace {
// QMacStyle's layout rect for a push button starts 4 px lower and ends 8 px higher.
class NativeLayoutMargins : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const override
    {
        return element == SE_PushButtonLayoutItem ? option->rect.adjusted(0, 4, 0, -8)
                                                  : QProxyStyle::subElementRect(element, option, widget);
    }
};
}

void TestSessionsWindow::railButtonsIgnoreNativeLayoutMargins()
{
    NativeLayoutMargins style(QStyleFactory::create("Fusion"));
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); QTest::qWait(50);
    // A window stylesheet would hide the native metric; apply it directly.
    window.setStyleSheet({});
    auto *sidebar = window.findChild<QWidget *>("sidebar"); auto *side = sidebar->layout();
    for (auto *button : sidebar->findChildren<QPushButton *>(Qt::FindDirectChildrenOnly)) button->setStyle(&style);
    side->activate(); QTest::qWait(20);
    QList<QWidget *> rail;
    for (int i = 0; i < side->count(); ++i)
        if (auto *widget = side->itemAt(i)->widget(); widget && widget->isVisible()) rail << widget;
    QVERIFY(rail.size() >= 7);
    for (int i = 1; i < rail.size(); ++i)
        QVERIFY2(rail[i - 1]->geometry().bottom() < rail[i]->geometry().top(),
                 qPrintable(QString("%1 overlaps %2").arg(rail[i - 1]->objectName(), rail[i]->objectName())));
}

void TestSessionsWindow::sessionsRailResetsFiltersAndShowsAttention()
{
    SessionsWindow window(program()); auto state=fleet(); window.setFleet(state); window.show(); QTest::qWait(70);
    QPushButton *sessions=nullptr;
    for (auto *button:window.findChildren<QPushButton *>()) if (button->property("glyph")=="sessions" && button->objectName()=="railButton") sessions=button;
    QVERIFY(sessions);
    auto *brand=window.findChild<QPushButton *>("brandMark");
    QVERIFY(brand->property("attentionCount").toInt()>0);
    QVERIFY(window.findChild<QLabel *>("railAttentionBadge")->isVisible());
    auto *search=window.findChild<QLineEdit *>("search");search->setText("no matching sessions");
    for (auto *button:window.findChildren<QPushButton *>()) if (button->property("filter")=="active") button->click();
    sessions->click();QVERIFY(search->text().isEmpty());
    for (auto *button:window.findChildren<QPushButton *>()) if (button->property("filter")=="all") QVERIFY(button->isChecked());
    auto box=state.local();for(auto &session:box.sessions){session.phase="idle";session.activity="idle";session.unreadReply=false;}
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    for(const auto &host:state.peerNames()){auto peer=*state.peer(host);for(auto &session:peer.sessions){session.phase="idle";session.activity="idle";session.unreadReply=false;}state.setPeer(peer,QDateTime::currentMSecsSinceEpoch());}
    window.setFleet(state);QCOMPARE(brand->property("attentionCount").toInt(),0);
    QVERIFY(window.findChild<QLabel *>("railAttentionBadge")->isHidden());
}

void TestSessionsWindow::subagentGroupsAndUnavailableActivity()
{
    auto state = fleet(); auto box = state.local(); auto &session = box.sessions[0];
    session.subagentSource = "hook_profiles"; session.subagentTotalCount = 3; session.subagentActiveCount = 3;
    session.subagentCountsComplete = false;
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); QTest::qWait(60);
    auto *list = window.findChild<QListWidget *>("sessionList"); QCOMPARE(list->item(0)->data(Qt::UserRole + 7).toString(), QString("?"));
    QVERIFY(list->item(0)->toolTip().isEmpty());
    QVERIFY(list->item(0)->data(Qt::AccessibleTextRole).toString().contains("incomplete counts"));
    auto *client = window.findChild<HgsClient *>();
    QJsonObject details{{"tracked", true}, {"conversation_id", "conversation-one"}, {"subagent_source", "hook_profiles"},
        {"subagent_counts_complete", true}, {"subagent_active_count", 2}, {"subagent_total_count", 8},
        {"subagents", QJsonObject{{"collapsed", QJsonObject{{"label", "Do not invent a child"}, {"state", "working"}}}}},
        {"subagent_groups", QJsonObject{
            {"code", QJsonObject{{"label", "code"}, {"active_count", 2}, {"total_count", 5}, {"detail", "Review implementation"}}},
            {"review", QJsonObject{{"label", "review"}, {"active_count", 0}, {"total_count", 3}, {"detail", "Read final diff"}}}}}};
    client->inspectionReady({}, session.name, details);
    auto *roster = window.findChild<QTextBrowser *>("subagents"); auto text = roster->toPlainText();
    QVERIFY(text.contains("Profile groups")); QVERIFY(text.contains("2 active / 8 observed runs"));
    QVERIFY(text.contains("code")); QVERIFY(text.contains("Read final diff")); QVERIFY(!text.contains("Do not invent a child"));
    auto *showCompleted = window.findChild<QCheckBox *>("workspaceShowCompletedSubagents"); QVERIFY(showCompleted);
    showCompleted->setChecked(true); QVERIFY(roster->toPlainText().contains("Read final diff"));
    showCompleted->setChecked(false);
    details["subagent_counts_complete"] = false; client->inspectionReady({}, session.name, details);
    text = roster->toPlainText(); QVERIFY(text.contains("incomplete counts")); QVERIFY(!text.contains("2/5")); QVERIFY(text.contains("Read final diff"));
    details["subagent_groups"] = QJsonObject{}; details["subagents"] = QJsonObject{};
    client->inspectionReady({}, session.name, details);
    QVERIFY(roster->toPlainText().contains("Exact subagent counts are unavailable"));
    QVERIFY(!roster->toPlainText().contains("No subagent activity has been reported yet"));
    details["subagent_source"] = "unavailable"; client->inspectionReady({}, session.name, details);
    QVERIFY(roster->toPlainText().contains("activity is unavailable")); QVERIFY(!roster->toPlainText().contains("0 active"));
}

void TestSessionsWindow::batchActionsAndReturnToSessions()
{
    SessionsWindow window(program()); auto state = fleet(); window.setFleet(state); window.show();
    QCOMPARE(window.windowTitle(), QString("Zerus"));
    auto *button = window.findChild<QPushButton *>("batchActions"); QVERIFY(button);
    auto *menu = button->menu(); QVERIFY(menu);
    auto refreshMenu = [&]() { QMetaObject::invokeMethod(menu, "aboutToShow"); };
    refreshMenu();
    QCOMPARE(menu->actions().size(), 5); // Machines, separator, mark-all-read and archive cleanup.
    auto *clear=window.findChild<QAction *>("clearArchive");QVERIFY(menu->actions().contains(clear));
    QVERIFY(clear->isVisible());QVERIFY(!clear->isEnabled()); // No archived entries yet.
    auto *arch = menu->actions()[0]->menu(); auto *mac = menu->actions()[1]->menu();
    QVERIFY(!arch->actions().first()->isEnabled()); // Busy and untracked sessions block pause-all.
    QVERIFY(arch->actions().last()->isEnabled());
    QVERIFY(!mac->actions().first()->isEnabled()); // Awaiting approval is not idle.
    auto ready = *state.peer("mac"); ready.sessions[0].activity = "idle";
    state.setPeer(ready, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state); refreshMenu();
    mac = menu->actions()[1]->menu(); QVERIFY(mac->actions().first()->isEnabled());
    auto *client = window.findChild<HgsClient *>(); QSignalSpy done(client, &HgsClient::writeDone);
    mac->actions().first()->trigger(); refreshMenu();
    QVERIFY(!menu->actions()[0]->menu()->actions().last()->isEnabled()); // Another write is pending.
    QTRY_COMPARE(done.size(), 1); QVERIFY(done[0][1].toBool());
    QCOMPARE(done[0][2].toString().trimmed(), QString("@mac\npause\n--all"));
    refreshMenu(); menu->actions()[0]->menu()->actions().last()->trigger();
    QTRY_COMPARE(done.size(), 2); QVERIFY(done[1][1].toBool());
    QCOMPARE(done[1][2].toString().trimmed(), QString("resume\n--all\n-d"));
    state.setPeer(ready, QDateTime::currentMSecsSinceEpoch() - FleetState::kPeerStaleMs - 1);
    window.setFleet(state); refreshMenu(); QVERIFY(!menu->actions()[1]->isEnabled());
    window.setConnectionError("offline"); refreshMenu(); QVERIFY(!menu->actions()[0]->isEnabled());
    for (int i = 0; i < 5; ++i) refreshMenu();
    QCOMPARE(menu->findChildren<QMenu *>().size(), 2);
    window.showProjects("mac");
    auto *projects = window.findChild<ProjectsDialog *>(); QVERIFY(projects->isVisible());
    window.showSessionList(); QVERIFY(!projects->isVisible());
    auto *list = window.findChild<QListWidget *>("sessionList"); QVERIFY(list->isVisible());
    window.findChild<QLineEdit *>("search")->setText("nothing matches");
    window.showProjects(); window.showSession({}, "codex/website/navigation");
    QVERIFY(!projects->isVisible());
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QString("\ncodex/website/navigation"));
}

void TestSessionsWindow::archiveFiltersIdentityRestoreAndForget()
{
    auto state = fleet(); auto local = state.local();
    auto old = local.sessions[0]; old.state = "archived"; old.archiveId = "older"; old.archivedAt = 1790928000;
    auto recent = old; recent.archiveId = "recent"; recent.archivedAt += 10;
    local.sessions.append(old); local.sessions.append(recent); state.setLocal(local, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    window.setFleet(state); keepDetailsPollsOut(window); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList");
    auto filter = [&](const QString &id) -> QPushButton * {
        for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == id) return button;
        return nullptr;
    };
    QCOMPARE(list->count(), 5); QVERIFY(filter("all")->text().endsWith("5"));
    QVERIFY(filter("archived")->text().endsWith("2"));
    filter("paused")->click(); QCOMPARE(list->count(), 1);
    filter("archived")->click(); QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->data(Qt::UserRole).toString(), QString("\ncodex/hgs/dashboard\nrecent"));
    QCOMPARE(list->item(1)->data(Qt::UserRole).toString(), QString("\ncodex/hgs/dashboard\nolder"));
    list->setCurrentRow(0); // Filtering alone keeps the live conversation open.
    // An archived session keeps Restore in the header; the menu names the same action.
    auto *restore = window.findChild<QPushButton *>("restoreSessionAction"); QVERIFY(restore->isVisible());
    QCOMPARE(restore->accessibleName(), QString("Restore session")); QVERIFY(!restore->isEnabled());
    QCOMPARE(window.findChild<QAction *>("pauseAction")->text(), QString("Restore session"));
    QVERIFY(!window.findChild<QPushButton *>("clearSessionAction")->isVisible());
    QVERIFY(window.findChild<QLabel *>("hint")->text().contains("uses this name"));
    QVERIFY(!window.findChild<QAction *>("archiveSessionAction")->isVisible());
    QSignalSpy terminal(&window, &SessionsWindow::sessionActivated);
    QSignalSpy writes(client, &HgsClient::writeDone);
    QTest::keyClick(list, Qt::Key_Return); QCOMPARE(terminal.size(), 0); QCOMPARE(writes.size(), 0);
    QTest::mouseDClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(0)).center());
    QCOMPARE(terminal.size(), 0); QCOMPARE(writes.size(), 0);
    // Wait for this archive's fixture reply before injecting late replies. A
    // fixed delay can let a slow subprocess replace the injected details. Selecting
    // the archive started the only inspection of it: with polls kept out, none
    // follows that reply to replace the injected prompt with the fixture's details.
    QTRY_VERIFY(std::any_of(inspected.cbegin(), inspected.cend(), [&](const QList<QVariant> &reply) {
        return reply[0].toString().isEmpty() && reply[1].toString() == recent.name
            && reply[3].toString() == recent.archiveId;
    }));
    auto *activity = window.findChild<QTextBrowser *>("activity");
    QJsonObject details{{"tracked", true}, {"conversation_id", "conversation-one"}, {"prompt", "wrong live request"}, {"events", QJsonArray{}}, {"cursor", 0}};
    client->inspectionReady({}, recent.name, details); QVERIFY(!activity->toPlainText().contains("wrong live request"));
    details["prompt"] = "wrong older archive";
    client->inspectionReady({}, recent.name, details, "older"); QVERIFY(!activity->toPlainText().contains("wrong older archive"));
    details["prompt"] = "selected archived request";
    client->inspectionReady({}, recent.name, details, "recent"); QTRY_VERIFY(activity->toPlainText().contains("selected archived request"));
    client->inspectionFailed({}, recent.name, "wrong failure", "older");
    QVERIFY(!window.findChild<QLabel *>("hint")->text().contains("wrong failure"));
    // A paused binding occupies the name just as a live terminal does.
    local.sessions[0].state = "paused"; state.setLocal(local, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(!restore->isEnabled());
    // Free the current name; Archive restoration still requires its dedicated button.
    local.sessions.removeFirst(); state.setLocal(local, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(restore->isEnabled()); restore->click(); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QString("resume\ncodex/hgs/dashboard\n--archive\nrecent\n-d"));
    QVERIFY(filter("all")->isChecked()); QCOMPARE(terminal.size(), 0);
    auto restored = recent; restored.state = "running"; restored.archiveId.clear(); restored.archivedAt = 0;
    local.sessions.removeLast(); local.sessions.append(restored);
    state.setLocal(local, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QString("\ncodex/hgs/dashboard"));
    filter("archived")->click(); QCOMPARE(list->count(), 1); list->setCurrentRow(0);
    QVERIFY(!restore->isEnabled()); // The newly restored terminal occupies the name.
    QTimer::singleShot(0, [&]() {
        if (auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) dialog->button(QMessageBox::Yes)->click();
    });
    window.findChild<QAction *>("forgetSessionAction")->trigger(); QTRY_COMPARE(writes.size(), 2);
    QCOMPARE(writes[1][2].toString(), QString("kill\ncodex/hgs/dashboard\n--archive\nolder"));
    // Archive-only machines never enable Resume all.
    BoxState archiveOnly; archiveOnly.host = "arch"; archiveOnly.ok = true; archiveOnly.sessions = {old};
    FleetState finalState; finalState.setLocal(archiveOnly, QDateTime::currentMSecsSinceEpoch()); window.setFleet(finalState);
    auto *menu = window.findChild<QPushButton *>("batchActions")->menu(); QMetaObject::invokeMethod(menu, "aboutToShow");
    QCOMPARE(menu->actions()[0]->menu()->actions().last()->text(), QString("Resume all in background (0)"));
    QVERIFY(!menu->actions()[0]->menu()->actions().last()->isEnabled());
}

void TestSessionsWindow::savedSessionsCanMoveToArchive()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show();
    window.showSession({}, "codex/website/navigation");
    auto *action = window.findChild<QAction *>("archiveSessionAction"); QVERIFY(action->isVisible());
    auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
    action->trigger(); QTRY_COMPARE(writes.size(), 1);
    QCOMPARE(writes[0][2].toString(), QString("archive\ncodex/website/navigation"));
    window.showSession({}, "codex/hgs/dashboard"); QVERIFY(!action->isVisible());
    action->trigger(); QTest::qWait(25); QCOMPARE(writes.size(), 1); // Live sessions cannot be archived from this action.
}

void TestSessionsWindow::archivingLastWorktreeSessionOffersCleanup()
{
    for (const bool shared : {false, true}) {
        auto state = fleet(); auto box = state.local();
        for (auto &session : box.sessions) {
            if (session.name == "codex/website/navigation") { session.gitWorktree = true; session.gitRoot = "/workspace/website-nav"; session.gitCommonDir = "/workspace/website/.git"; }
            else if (shared && session.name == "codex/hgs/dashboard") session.gitRoot = "/workspace/website-nav";
        }
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
        SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, "codex/website/navigation");
        auto *offer = window.findChild<QPushButton *>("archivedWorktreeCleanup"); QVERIFY(!offer->isVisible());
        auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
        window.findChild<QAction *>("archiveSessionAction")->trigger(); QTRY_COMPARE(writes.size(), 1); QVERIFY(writes[0][1].toBool());
        if (shared) { QTest::qWait(50); QVERIFY(!offer->isVisible()); continue; } // Another session still uses the checkout.
        QTRY_VERIFY(offer->isVisible()); QVERIFY(offer->toolTip().contains("/workspace/website-nav")); bool opened = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("worktreeBatchCleanupDialog"); QVERIFY(dialog); QTimer::singleShot(3000, dialog, [dialog] { dialog->done(0); });
            QCOMPARE(dialog->windowTitle(), QString("Review cleanup"));
            const auto target = dialog->findChild<QTreeWidget *>("batchCleanupList")->topLevelItem(0)->data(0,Qt::UserRole).toJsonObject();
            QCOMPARE(target["row"].toObject()["path"].toString(),QString("/workspace/website-nav")); opened = true; dialog->reject();
        });
        offer->click(); QVERIFY(opened); QVERIFY(!offer->isVisible());
    }
}

void TestSessionsWindow::terminatingWorktreeSessionOffersRemainingCleanupAndConsumesExactNativeOutcome()
{
    auto state=fleet();auto box=state.local();
    for(auto &session:box.sessions)if(session.name=="codex/hgs/dashboard"){session.gitWorktree=true;session.gitRoot="/workspace/owned";session.gitCommonDir="/workspace/project/.git";}
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());SessionsWindow window(program());window.setFleet(state);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();auto *offer=window.findChild<QPushButton *>("archivedWorktreeCleanup");QSignalSpy writes(client,&HgsClient::writeDone);
    QTimer::singleShot(0,&window,[&]{auto *confirm=window.findChild<QMessageBox *>("terminateSessionConfirm");QVERIFY(confirm);confirm->done(QMessageBox::Yes);});
    window.findChild<QAction *>("forgetSessionAction")->trigger();QTRY_COMPARE(writes.size(),1);QTRY_VERIFY(offer->isVisible());QVERIFY(offer->toolTip().contains("/workspace/owned"));
    client->worktreeCleanupFinished({},"terminate codex/hgs/dashboard",{{"status","removed"},{"path","/wrong"},{"common_dir","/workspace/project/.git"}});QVERIFY(offer->isVisible());
    client->worktreeCleanupFinished({},"terminate codex/hgs/dashboard",{{"status","kept"},{"path","/workspace/owned"},{"common_dir","/workspace/project/.git"},{"reason","Uncommitted changes remain"}});
    QVERIFY(offer->isVisible());QVERIFY(offer->toolTip().contains("Uncommitted changes"));
    client->worktreeCleanupFinished({},"terminate codex/hgs/dashboard",{{"status","removed"},{"path","/workspace/owned"},{"common_dir","/workspace/project/.git"}});QVERIFY(!offer->isVisible());
}

void TestSessionsWindow::nativeCleanupCanSupplyAnUncachedFolderAndArchiveTransitionsExpireOnlyTheirRepository()
{
    auto state=fleet();auto box=state.local();box.sessions[0].runId="owned-run";box.sessions[0].conversationId="owned-conversation";box.sessions[0].gitWorktree=true;box.sessions[0].gitRoot="/owned";box.sessions[0].gitCommonDir="/repo/.git";
    state.setLocal(box,QDateTime::currentMSecsSinceEpoch());SessionsWindow window(program());window.setFleet(state);window.show();
    auto *client=window.findChild<HgsClient *>();QSignalSpy expired(client,&HgsClient::worktreeCacheInvalidated);
    auto completed=box;completed.sessions[0].state="archived";completed.sessions[0].gitRoot.clear();completed.sessions[0].gitWorktree=false;completed.sessions[0].runId="other-run";
    auto replacement=state;replacement.setLocal(completed,QDateTime::currentMSecsSinceEpoch());window.setFleet(replacement);QCOMPARE(expired.size(),0);
    window.setFleet(state);completed.sessions[0].runId="owned-run";replacement.setLocal(completed,QDateTime::currentMSecsSinceEpoch());window.setFleet(replacement);QCOMPARE(expired.size(),1);QCOMPARE(expired[0][1].toString(),QString("/repo/.git"));
    window.setFleet(state);window.showSession({},"codex/website/navigation");QSignalSpy writes(client,&HgsClient::writeDone);window.findChild<QAction *>("archiveSessionAction")->trigger();QTRY_COMPARE(writes.size(),1);
    auto *offer=window.findChild<QPushButton *>("archivedWorktreeCleanup");QVERIFY(!offer->isVisible());
    client->worktreeCleanupFinished({},"archive codex/website/navigation",{{"status","kept"},{"path","/uncached-worktree"},{"common_dir","/repo/.git"},{"reason","Local files remain"}});QVERIFY(offer->isVisible());QVERIFY(offer->toolTip().contains("Local files remain"));
}

void TestSessionsWindow::forkValidationCancelAndPinnedPayload()
{
    auto state = fleet(); auto box = state.local();
    box.sessions[0].runId = "source-run";
    for (int i = 1; i <= 3; ++i) {
        auto collision = box.sessions[0];
        collision.tag = i == 1 ? "dashboard-fork" : QString("dashboard-fork-%1").arg(i);
        collision.name = "codex/hgs/" + collision.tag; collision.runId = QString("collision-%1").arg(i);
        collision.state = i == 3 ? "archived" : i == 2 ? "paused" : "running";
        collision.archiveId = i == 3 ? "occupied-archive-name" : "";
        box.sessions.append(collision);
    }
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    const QJsonObject details{{"tracked", true}, {"run_id", "source-run"}, {"conversation_id", "conversation-one"},
        {"fork_supported", true}, {"runtime_state", "live"}, {"activity", "idle"}, {"phase", "idle"},
        {"last_event_at", 2000000000}, {"events", QJsonArray{}}, {"cursor", 0}};
    // Polls (also those the dialogs' loops deliver) keep reporting fork support.
    const auto fixture = programInspecting("fork-validation-hgs", details); QVERIFY(!fixture.isEmpty());
    SessionsWindow window(fixture); window.setFleet(state); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    QSignalSpy writes(client, &HgsClient::writeDone); QTRY_VERIFY(!inspected.isEmpty());
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    auto *action = window.findChild<QAction *>("forkSessionAction"); QVERIFY(action && action->isEnabled());
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QTimer::singleShot(1000, dialog, &QDialog::reject);
        QCOMPARE(dialog->objectName(), QString("forkSessionDialog"));
        auto *name = dialog->findChild<QLineEdit *>("forkSessionName"); QVERIFY(name);
        QCOMPARE(name->text(), QString("dashboard-fork-4"));
        auto *create = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok); QVERIFY(create->isEnabled());
        for (const QString &invalid : {QString(), QString(" leading"), QString("a/b"), QString("dashboard-fork"), QString("dashboard-fork-2"), QString("dashboard-fork-3")}) {
            name->setText(invalid); QVERIFY(!create->isEnabled());
        }
        name->setText("Новая ветка"); QVERIFY(create->isEnabled()); dialog->reject();
    });
    action->trigger(); QCOMPARE(writes.size(), 0);
    QCOMPARE(window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString(), QString("\ncodex/hgs/dashboard"));
    client->inspectionReady({}, "codex/hgs/dashboard", details);
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("forkSessionName")->setText("Новая ветка");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    // The fork has started; its fixture process may take a while under load.
    action->trigger(); QVERIFY(!action->isEnabled()); QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 1, 15000);
    QVERIFY(writes[0][1].toBool());
    QCOMPARE(writes[0][2].toString(), QString("fork\ncodex/hgs/dashboard\n-n\nНовая ветка\n-d\n--expected-run-id\nsource-run\n--expected-conversation-id\nconversation-one"));
}

void TestSessionsWindow::forkKeepsSourceAndGroup_data()
{
    QTest::addColumn<QString>("sourceState");
    QTest::newRow("running") << QString("running");
    QTest::newRow("paused-filter") << QString("paused");
    QTest::newRow("archive-filter") << QString("archived");
}

void TestSessionsWindow::forkKeepsSourceAndGroup()
{
    QFETCH(QString, sourceState);
    SessionInfo original; original.name = "codex/project/topic"; original.tag = "topic";
    original.cmd = "codex"; original.project = "project"; original.tracked = true; original.resumable = true;
    original.runId = "original-run"; original.state = sourceState; original.activity = "idle"; original.phase = "idle";
    original.archiveId = sourceState == "archived" ? "source-archive" : ""; original.cwd = "/work/project";
    original.prompt = "Original public request";
    auto sibling = original; sibling.name = "codex/project/tail"; sibling.tag = "tail"; sibling.runId = "sibling-run";
    sibling.archiveId.clear(); sibling.state = "running";
    SessionOrganization org; const auto group = org.createGroup("My release");
    const auto sourceIdentity = org.observe("arch", original.name, original.runId, original.archiveId);
    const auto siblingIdentity = org.observe("arch", sibling.name, sibling.runId, {});
    org.moveSession(sourceIdentity, group); org.moveSession(siblingIdentity, group);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    BoxState box; box.host = "arch"; box.ok = true; box.sessions = {original, sibling};
    FleetState state; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    const QJsonObject details{{"tracked", true}, {"run_id", original.runId},
        {"conversation_id", "conversation-one"}, {"fork_supported", true}, {"cwd", original.cwd},
        {"activity", "idle"}, {"phase", "idle"}, {"last_event_at", 2000000000}, {"events", QJsonArray{}}, {"cursor", 0}};
    // Polls (also those the dialog's loop delivers) keep reporting fork support.
    const auto fixture = programInspecting(QString("fork-hgs-") + QTest::currentDataTag(), details); QVERIFY(!fixture.isEmpty());
    SessionsWindow window(fixture); window.setFleet(state); window.show();
    if (sourceState != "running") for (auto *button : window.findChildren<QPushButton *>())
        if (button->property("filter") == (sourceState == "archived" ? "archived" : "paused")) button->click();
    auto *list = window.findChild<SessionList *>("sessionList");
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(SessionRoles::Identity).toString() == sourceIdentity) { list->setCurrentRow(i); break; }
    QVERIFY(list->currentItem());
    QCOMPARE(list->currentItem()->data(SessionRoles::Identity).toString(), sourceIdentity);
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    QSignalSpy writes(client, &HgsClient::writeDone); QTRY_VERIFY(!inspected.isEmpty());
    client->inspectionReady({}, original.name, details, original.archiveId);
    auto *action = window.findChild<QAction *>("forkSessionAction"); QVERIFY(action->isEnabled());
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QLineEdit *>("forkSessionName")->text(), QString("topic-fork"));
        dialog->accept();
    });
    // The fork has started; its fixture process may take a while under load.
    action->trigger(); QVERIFY(!action->isEnabled()); QTRY_COMPARE_WITH_TIMEOUT(writes.size(), 1, 15000); QVERIFY(writes[0][1].toBool());
    if (sourceState == "archived") QVERIFY(writes[0][2].toString().contains("--archive\nsource-archive"));
    auto created = original; created.name = "codex/project/topic-fork"; created.tag = "topic-fork";
    created.runId = "independent-fork-run"; created.archiveId.clear(); created.state = "running";
    box.sessions.append(created); state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QTRY_VERIFY(list->currentItem());
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(), QString("\ncodex/project/topic-fork"));
    QVERIFY(!list->currentItem()->isHidden());
    const auto persisted = SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    const QString createdIdentity = "arch\ncodex/project/topic-fork";
    QCOMPARE(persisted.groupFor(sourceIdentity), group); QCOMPARE(persisted.groupFor(createdIdentity), group);
    QVERIFY(persisted.group(group));
    QCOMPARE(persisted.group(group)->sessions, QStringList({sourceIdentity, createdIdentity, siblingIdentity}));
    QVERIFY(!persisted.group(group)->collapsed);
    QTRY_COMPARE(window.findChild<QTabWidget *>("sessionDetailTabs")->currentWidget()->objectName(), QString("terminalView"));
    QCOMPARE(state.local().sessions[0].name, original.name); QCOMPARE(state.local().sessions[0].runId, original.runId);
    QCOMPARE(state.local().sessions[0].archiveId, original.archiveId); QCOMPARE(state.local().sessions[0].prompt, original.prompt);
    if (sourceState == "archived") {
        for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "archived") button->click();
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(SessionRoles::Identity).toString() == sourceIdentity) { list->setCurrentRow(i); break; }
        QVERIFY(list->currentItem()); QCOMPARE(list->currentItem()->data(SessionRoles::Identity).toString(), sourceIdentity);
    }
}

void TestSessionsWindow::forkDisabledForUnavailableSources()
{
    SessionsWindow window(program()); auto state = fleet(); window.setFleet(state); window.show();
    window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    QSignalSpy writes(client, &HgsClient::writeDone); QTRY_VERIFY(!inspected.isEmpty());
    auto *action = window.findChild<QAction *>("forkSessionAction"); QVERIFY(!action->isEnabled());
    QJsonObject details{{"tracked", true}, {"run_id", "source-run"}, {"conversation_id", "conversation-one"},
        {"fork_supported", false}, {"fork_reason", "Wait for Ready to fork"}, {"activity", "busy"}, {"phase", "tool"},
        {"last_event_at", 2000000000}, {"events", QJsonArray{}}, {"cursor", 0}};
    client->inspectionReady({}, "codex/hgs/dashboard", details); QVERIFY(!action->isEnabled());
    action->trigger(); QCOMPARE(writes.size(), 0); QVERIFY(!window.findChild<QDialog *>("forkSessionDialog"));
    details["fork_reason"] = "Provider does not support independent forks";
    client->inspectionReady({}, "codex/hgs/dashboard", details); QVERIFY(!action->isEnabled());
    QVERIFY(action->toolTip().contains("Provider"));
    details["fork_supported"] = true; details["activity"] = "idle"; details["phase"] = "idle";
    client->inspectionReady({}, "codex/hgs/dashboard", details); QVERIFY(action->isEnabled());
    auto box = state.local(); box.ok = false; state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QVERIFY(!action->isEnabled()); action->trigger(); QCOMPARE(writes.size(), 0);
    QVERIFY(!window.findChild<QDialog *>("forkSessionDialog"));
}

void TestSessionsWindow::renameValidationCancelAndSelection()
{
    auto state = fleet(); auto box = state.local();
    auto occupied = box.sessions[0]; occupied.name = "codex/hgs/taken"; occupied.tag = "taken"; box.sessions.append(occupied);
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *action = window.findChild<QAction *>("renameSessionAction"); QVERIFY(action->isEnabled()); // Busy is allowed.
    auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QTimer::singleShot(1000, dialog, &QDialog::reject);
        auto *name = dialog->findChild<QLineEdit *>("renameSessionName"); QCOMPARE(name->text(), QString("dashboard"));
        auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save); QVERIFY(!save->isEnabled());
        for (const auto &invalid : QStringList{"", " leading", "trailing ", "a/b", "a:b", "a.b", "a\tb", "taken"}) {
            name->setText(invalid); QVERIFY(!save->isEnabled());
            QVERIFY(!dialog->findChild<QLabel *>("renameError")->text().isEmpty());
        }
        name->setText("Новый план"); QVERIFY(save->isEnabled());
        QCOMPARE(dialog->findChild<QLabel *>("renameFullName")->text(), QString("codex/hgs/Новый план"));
        dialog->reject();
    });
    action->trigger(); QCOMPARE(writes.size(), 0);
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog); dialog->accept();
    });
    action->trigger(); QCOMPARE(writes.size(), 0); // Even a programmatic unchanged accept is a no-op.
    QTest::qWait(50);
    client->inspectionReady({}, "codex/hgs/dashboard", QJsonObject{{"tracked", true}, {"conversation_id", "conversation-one"},
        {"cursor", 500}, {"events", QJsonArray{QJsonObject{{"seq", 500}, {"type", "Stop"}, {"detail", "old activity"}}}}});
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("renameSessionName")->setText("Новый <план>");
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    });
    action->trigger(); QVERIFY(!window.findChild<QPushButton *>("more")->isEnabled());
    QTRY_COMPARE(writes.size(), 1); QVERIFY(writes[0][1].toBool());
    QCOMPARE(writes[0][2].toString(), QString("rename\ncodex/hgs/dashboard\ncodex/hgs/Новый <план>"));
    auto *activity = window.findChild<QTextBrowser *>("activity");
    const QJsonObject late{{"tracked", true}, {"conversation_id", "conversation-one"}, {"cursor", 501},
        {"events", QJsonArray{QJsonObject{{"seq", 501}, {"type", "Stop"}, {"detail", "stale rename response"}}}}};
    client->inspectionReady({}, "codex/hgs/dashboard", late);
    QVERIFY(!activity->toPlainText().contains("stale rename response"));
    box.sessions[0].name = "codex/hgs/Новый <план>"; box.sessions[0].tag = "Новый <план>";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    QCOMPARE(window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString(), QString("\ncodex/hgs/Новый <план>"));
    auto *title = window.findChild<QLabel *>("detailTitle"); QCOMPARE(title->accessibleName(), QString("General / Новый <план>"));
    QCOMPARE(title->textFormat(), Qt::PlainText);
    QTest::qWait(50);
    client->inspectionReady({}, box.sessions[0].name, QJsonObject{{"tracked", true}, {"conversation_id", "conversation-one"},
        {"cursor", 1}, {"events", QJsonArray{QJsonObject{{"seq", 1}, {"type", "Stop"}, {"detail", "new inspection starts at zero"}}}}});
    QTRY_VERIFY(activity->toPlainText().contains("new inspection starts at zero"));
    client->inspectionReady({}, "codex/hgs/dashboard", late);
    client->inspectionFailed({}, "codex/hgs/dashboard", "late old failure");
    QVERIFY(!activity->toPlainText().contains("stale rename response"));
    QVERIFY(!window.findChild<QLabel *>("hint")->text().contains("late old failure"));
}

void TestSessionsWindow::renameOtherSessionKinds()
{
    for (const QString kind : {QString("untracked"), QString("paused"), QString("stopped"), QString("archived")}) {
        const bool archived = kind == "archived", remote = kind == "stopped";
        SessionInfo session; session.name = "codex/project"; session.cmd = "codex"; session.project = "project";
        session.state = kind == "untracked" ? "running" : kind; session.tracked = kind != "untracked";
        session.archiveId = archived ? "archive-original" : "";
        BoxState box; box.host = remote ? "mac" : "arch"; box.ok = true; box.sessions = {session};
        if (archived) { auto live = session; live.name = "codex/project/new label"; live.state = "running"; live.archiveId.clear(); box.sessions.append(live); }
        FleetState state;
        if (remote) { BoxState local; local.host = "arch"; local.ok = true; state.setLocal(local, QDateTime::currentMSecsSinceEpoch()); state.setPeer(box, QDateTime::currentMSecsSinceEpoch()); }
        else state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
        SessionsWindow window(program()); window.setFleet(state); window.show();
        if (archived) {
            for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "archived") button->click();
            window.findChild<SessionList *>("sessionList")->setCurrentRow(0);
        }
        auto *action = window.findChild<QAction *>("renameSessionAction"); QVERIFY(action->isEnabled());
        auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
            QTimer::singleShot(1000, dialog, &QDialog::reject);
            auto *name = dialog->findChild<QLineEdit *>("renameSessionName"); QVERIFY(name->text().isEmpty());
            name->setText("new label"); auto *save = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
            QVERIFY(save->isEnabled()); save->click();
        });
        action->trigger(); QTRY_COMPARE(writes.size(), 1); QVERIFY(writes[0][1].toBool());
        QCOMPARE(writes[0][2].toString(), (remote ? QString("@mac\n") : QString()) + "rename\ncodex/project\ncodex/project/new label" + (archived ? "\n--archive\narchive-original" : ""));
        box.sessions[0].name = "codex/project/new label"; box.sessions[0].tag = "new label";
        if (remote) state.setPeer(box, QDateTime::currentMSecsSinceEpoch()); else state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
        window.setFleet(state);
        QCOMPARE(window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString(),
                 (remote ? QString("mac") : QString()) + "\ncodex/project/new label" + (archived ? "\narchive-original" : ""));
    }
}

void TestSessionsWindow::renameFailureKeepsSelection()
{
    QTemporaryDir dir; QFile fake(dir.filePath("hgs")); QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write("#!/usr/bin/env python3\nimport sys\nif sys.argv[1:2]==['inspect']:print('{\"events\":[],\"cursor\":0}')\n"
        "else:print('Name is already used by another session',file=sys.stderr);sys.exit(1)\n"); fake.close();
    QVERIFY(FakeCli::makeRunnable(fake.fileName()));
    SessionsWindow window(FakeCli::program(fake.fileName())); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); QSignalSpy writes(client, &HgsClient::writeDone);
    QTimer::singleShot(0, &window, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        dialog->findChild<QLineEdit *>("renameSessionName")->setText("racing collision"); dialog->accept();
    });
    window.findChild<QAction *>("renameSessionAction")->trigger(); QTRY_COMPARE(writes.size(), 1);
    QVERIFY(!writes[0][1].toBool()); QVERIFY(window.findChild<QPushButton *>("more")->isEnabled());
    QCOMPARE(window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString(), QString("\ncodex/hgs/dashboard"));
    QVERIFY(window.findChild<QLabel *>("notice")->text().contains("already used"));
}

void TestSessionsWindow::searchResultKeepsItsSessionAndSnapshot()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show();
    auto *search = window.findChild<QLineEdit *>("search"); search->setText("review");
    auto *results = window.findChild<QListWidget *>("searchResults"); QCOMPARE(results->count(), 1);
    results->setCurrentRow(0);
    auto *title = window.findChild<QLabel *>("detailTitle"); QVERIFY(title->text().contains("review"));
    window.setFleet(fleet()); QVERIFY(title->text().contains("review")); // Poll must not select hidden regular-list row.
    search->clear(); QVERIFY(window.findChild<QListWidget *>("sessionList")->isVisible());
    QCOMPARE(window.findChild<QListWidget *>("sessionList")->currentItem()->data(Qt::UserRole).toString(), QString("mac\nclaude/infra/review"));
}

void TestSessionsWindow::blankListContextMenu()
{
    SessionsWindow window(program()); FleetState empty; BoxState box; box.ok = true; box.host = "arch"; empty.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    window.setFleet(empty); window.show();
    auto *list = window.findChild<QListWidget *>("sessionList"); auto *menu = window.findChild<QMenu *>("sessionProjectsMenu");
    QVERIFY(menu); list->customContextMenuRequested(QPoint(20,80));
    QVERIFY(menu->isVisible()); QVERIFY(menu->actions().first()->text().contains("New session"));
    QVERIFY(menu->actions().at(1)->text().contains("New project")); menu->hide();
}

void TestSessionsWindow::contextMenuAcrossSessionPanelBackground()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.resize(1000, 750); window.show();
    auto *filter = window.findChild<MachineFilter *>("machineFilterChips"); filter->setSelection({"mac"});
    QTest::qWait(20);
    auto *list = window.findChild<SessionList *>("sessionList");
    auto *panel = window.findChild<QWidget *>("sessionListPanel");
    auto *groups = window.findChild<QMenu *>("sessionProjectsMenu"); QVERIFY(groups);
    auto *session = window.findChild<QMenu *>("sessionContextMenu");
    const auto sendContext = [](QWidget *target, const QPoint &point) {
        QContextMenuEvent event(QContextMenuEvent::Mouse, point, target->mapToGlobal(point));
        QApplication::sendEvent(target, &event);
    };
    const auto expectGroups = [&](QWidget *target, const QPoint &point) {
        sendContext(target, point);
        const bool correct = groups->isVisible() && !session->isVisible();
        groups->hide(); session->hide(); return correct;
    };
    QCOMPARE(list->count(), 1);
    const auto row = list->visualItemRect(list->item(0));
    const int panelY = list->viewport()->mapTo(panel, row.center()).y();
    QVERIFY2(expectGroups(panel, QPoint(1, panelY)), "Left panel gutter");
    QVERIFY2(expectGroups(panel, QPoint(panel->width() - 2, panelY)), "Right panel gutter");
    QVERIFY2(expectGroups(list->viewport(), QPoint(row.left(), row.center().y())), "Left card gutter");
    QVERIFY2(expectGroups(list->viewport(), QPoint(row.right(), row.center().y())), "Right card gutter");
    QVERIFY2(expectGroups(list->viewport(), QPoint(row.center().x(), row.top())), "Gap above card");
    QVERIFY2(expectGroups(list->viewport(), QPoint(20, row.bottom() + 20)), "Below last session");
    QVERIFY2(expectGroups(filter, QPoint(filter->width() - 2, 10)), "Empty tag strip");
    auto *chip = filter->findChild<QPushButton *>("machineFilterChip"); QVERIFY(chip);
    QVERIFY2(expectGroups(chip, chip->rect().center()), "Machine filter chip");
    QCOMPARE(filter->selection(), QSet<QString>{"mac"});
    sendContext(list->viewport(), row.center()); QVERIFY(session->isVisible()); QVERIFY(!groups->isVisible()); session->hide();

    // Text fields retain their native editing menu instead of the panel menu.
    auto *search = window.findChild<QLineEdit *>("search"); bool nativeMenu = false;
    QTimer::singleShot(0, &window, [&] {
        auto *popup = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        nativeMenu = popup && popup != groups && popup != session && !groups->isVisible();
        if (popup) popup->close();
    });
    sendContext(search, search->rect().center()); QCoreApplication::processEvents();
    QVERIFY(nativeMenu);
}

void TestSessionsWindow::sessionListCollapsesIntoWorkingStrip()
{
    QSettings().setValue("workspace/expandSessionsOnHover", false);
    if (!qEnvironmentVariable("HGS_STRIP_PREVIEW").isEmpty()) QSettings().setValue("workspace/theme", "dark");
    auto window = std::make_unique<SessionsWindow>(program()); window->resize(1280, 860); window->setFleet(fleet()); window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    auto *panel = window->findChild<QWidget *>("sessionListPanel"), *slot = window->findChild<QWidget *>("sessionListSlot");
    auto *toggle = window->findChild<QPushButton *>("sessionPanelToggle"); auto *list = window->findChild<SessionList *>("sessionList");
    QVERIFY(panel && slot && toggle && list);
    QTRY_COMPARE(panel->width(), slot->width()); QVERIFY(slot->width() >= 270);
    const int docked = slot->width();
    auto *detail = window->findChild<QStackedWidget *>("detail"); const int detailWidth = detail->width();
    const auto preview = qEnvironmentVariable("HGS_STRIP_PREVIEW"); if (!preview.isEmpty()) QDir().mkpath(preview);
    const auto shot = [&](QWidget *target, const QString &name) { if (!preview.isEmpty()) QVERIFY(target->grab().save(preview + "/" + name)); };
    shot(window.get(), "docked.png");

    toggle->click();
    if (!preview.isEmpty()) { QTest::qWait(70); shot(window.get(), "collapsing.png"); }
    QTRY_COMPARE(list->property("expansion").toReal(), 0.0);
    QCOMPARE(panel->width(), SessionStrip::width(true)); QCOMPARE(slot->width(), SessionStrip::width(true)); QVERIFY(detail->width() > detailWidth);
    QVERIFY(QSettings().value("workspace/sessionsCollapsed").toBool());
    QVERIFY(!window->findChild<QLineEdit *>("search")->isVisible()); QVERIFY(window->findChild<QPushButton *>("sessionStripSearch")->isVisible());
    // The strip keeps the active filter and the most urgent one, together as wide
    // as a tile, under a search button of the same width.
    const auto shownFilters = [&] {
        QStringList ids; for (auto *filter : window->findChildren<QPushButton *>("sessionFilter")) if (filter->isVisible()) ids << filter->property("filter").toString();
        return ids;
    };
    const auto filterButton = [&](const QString &id) {
        for (auto *filter : window->findChildren<QPushButton *>("sessionFilter")) if (filter->property("filter") == id) return filter;
        return static_cast<QPushButton *>(nullptr);
    };
    QCOMPARE(shownFilters(), QStringList({"all", "attention"}));
    {
        auto *all = filterButton("all"), *attention = filterButton("attention"), *search = window->findChild<QPushButton *>("sessionStripSearch");
        const auto *first = list->item(0)->data(SessionRoles::Header).toBool() ? list->item(1) : list->item(0);
        const QRect tile = sessionCardRect(list->visualItemRect(first)).translated(list->viewport()->mapTo(panel, QPoint()));
        const auto edges = [panel](QWidget *w) { const QPoint at = w->mapTo(panel, QPoint()); return std::pair{at.x(), at.x() + w->width()}; };
        QTRY_COMPARE(edges(search), std::pair(tile.x(), tile.x() + tile.width()));
        QCOMPARE(edges(all).first, tile.x()); QCOMPARE(edges(attention).second, tile.x() + tile.width());
        QVERIFY(edges(attention).first > edges(all).second);
    }
    auto *summary = window->findChild<QLabel *>("listSummary");
    QVERIFY(summary->toolTip().contains("shown")); QCOMPARE(summary->text(), summary->toolTip().section(' ', 0, 0));
    QCOMPARE(toggle->property("glyph").toString(), QString("expand-sessions"));

    // The strip keeps every row, as narrow as the strip, and still selects sessions.
    int target = -1;
    for (int i = 0; i < list->count(); ++i) {
        const auto *row = list->item(i);
        QVERIFY(list->visualItemRect(row).width() <= list->viewport()->width());
        // Each session is a square tile.
        if (!row->data(SessionRoles::Header).toBool() && !row->isHidden()) {
            const QRect card = sessionCardRect(list->visualItemRect(row)); QCOMPARE(card.width(), card.height());
        }
        if (!row->data(SessionRoles::Header).toBool() && row != list->currentItem() && !row->isHidden()) target = i;
    }
    QVERIFY(target >= 0);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, list->visualItemRect(list->item(target)).center());
    QCOMPARE(list->currentRow(), target);
    QImage strip = list->viewport()->grab().toImage(); QVERIFY(!strip.isNull());
    shot(window.get(), "collapsed.png");
    // A tile's frame takes its state's color; the card's attention edge stays away.
    const bool dark = list->property("hgsDark").toBool(); int framed = 0;
    const auto near = [](QColor a, QColor b) { return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) <= 24; };
    for (int i = 0; i < list->count(); ++i) {
        const auto *row = list->item(i);
        if (row->isHidden() || row->data(SessionRoles::Header).toBool() || !row->data(SessionRoles::ChildId).toString().isEmpty()) continue;
        const auto status = SessionDelegate::statusOf(list->model()->index(i, 0), row == list->currentItem());
        const QColor edge = SessionStatusBadge::edge(status.kind, dark);
        if (!edge.isValid()) continue;
        const QRect card = sessionCardRect(list->visualItemRect(row)); ++framed;
        QVERIFY2(near(strip.pixelColor(card.x(), card.center().y()), edge), qPrintable(row->data(SessionRoles::Title).toString()));
        QVERIFY(!near(strip.pixelColor(card.x() + 4, card.center().y()), IdentityBadges::attentionColor(dark, status.kind == SessionStatusBadge::Error)));
    }
    QVERIFY(framed >= 3);
    // Taking the urgent filter offers the next one: active sessions.
    filterButton("attention")->click();
    QTRY_COMPARE(shownFilters(), QStringList({"attention", "active"}));
    filterButton("all")->click(); QTRY_COMPARE(shownFilters(), QStringList({"all", "attention"}));

    // The collapsed state survives a restart and keeps the docked width for later.
    window.reset(); QSettings().setValue("workspace/sessionsWidth", 360);
    window = std::make_unique<SessionsWindow>(program()); window->resize(1280, 860); window->setFleet(fleet()); window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    panel = window->findChild<QWidget *>("sessionListPanel"); slot = window->findChild<QWidget *>("sessionListSlot");
    QTRY_COMPARE(panel->width(), SessionStrip::width(true));
    window->findChild<QPushButton *>("sessionPanelToggle")->click();
    QTRY_COMPARE(slot->width(), 360); QTRY_COMPARE(panel->width(), 360); QVERIFY(docked != 360);
    QVERIFY(!QSettings().value("workspace/sessionsCollapsed").toBool());
    QCOMPARE(window->findChild<SessionList *>("sessionList")->property("expansion").toReal(), 1.0);
}

void TestSessionsWindow::collapsedStripExpandsOverContentOnlyWhenEnabled()
{
    QSettings().setValue("workspace/sessionsCollapsed", true);
    {
        SessionsWindow window(program());
        auto *option = window.findChild<QCheckBox *>("workspaceExpandSessionsOnHover"); QVERIFY(option); QVERIFY(option->isChecked());
    }
    QSettings().setValue("workspace/expandSessionsOnHover", false);
    {
        SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *option = window.findChild<QCheckBox *>("workspaceExpandSessionsOnHover"); QVERIFY(!option->isChecked());
        auto *panel = window.findChild<QWidget *>("sessionListPanel");
        QTRY_COMPARE(panel->width(), SessionStrip::width(true));
        QTest::mouseMove(panel, QPoint(30, 300)); QTest::qWait(450);
        QCOMPARE(panel->width(), SessionStrip::width(true));
    }
    QSettings().setValue("workspace/expandSessionsOnHover", true);
    if (!qEnvironmentVariable("HGS_STRIP_PREVIEW").isEmpty()) QSettings().setValue("workspace/theme", "dark");
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.findChild<QCheckBox *>("workspaceExpandSessionsOnHover")->isChecked());
    auto *panel = window.findChild<QWidget *>("sessionListPanel"), *slot = window.findChild<QWidget *>("sessionListSlot");
    auto *detail = window.findChild<QStackedWidget *>("detail");
    QTRY_COMPARE(panel->width(), SessionStrip::width(true));
    const int detailWidth = detail->width();
    QTest::mouseMove(panel, QPoint(30, 300));
    QTRY_VERIFY(panel->width() >= 270);
    const auto preview = qEnvironmentVariable("HGS_STRIP_PREVIEW");
    if (!preview.isEmpty()) { QTest::qWait(300); QDir().mkpath(preview); QVERIFY(window.grab().save(preview + "/hover.png")); }
    QCOMPARE(slot->width(), SessionStrip::width(true)); QCOMPARE(detail->width(), detailWidth);
    // Nothing dims the conversation: only the panel and its edge shadow lie over it.
    auto *splitter = slot->parentWidget();
    for (auto *layer : panel->parentWidget()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (layer->isVisible() && layer != panel && layer != splitter && layer->geometry().intersects(splitter->geometry()))
            QVERIFY(layer->testAttribute(Qt::WA_TransparentForMouseEvents) && layer->width() <= 18);
    QCOMPARE(window.findChild<QPushButton *>("sessionPanelToggle")->property("glyph").toString(), QString("pin"));
    QTest::mouseMove(detail, QPoint(detail->width() - 40, 300));
    QTRY_COMPARE(panel->width(), SessionStrip::width(true));
    QVERIFY(QSettings().value("workspace/sessionsCollapsed").toBool());
}

void TestSessionsWindow::collapsingKeepsRowsInPlace()
{
    QSettings().setValue("workspace/expandSessionsOnHover", false);
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *panel = window.findChild<QWidget *>("sessionListPanel"); auto *list = window.findChild<SessionList *>("sessionList");
    auto *toggle = window.findChild<QPushButton *>("sessionPanelToggle"); auto *chips = window.findChild<MachineFilter *>("machineFilterChips");
    auto *projects = window.findChild<ProjectFilter *>("projectFilterChips");
    const auto top = [&] { return list->mapTo(panel, QPoint()).y(); };
    for (bool filtered : {false, true}) {
        if (filtered) { chips->setSelection({"mac"}); projects->setSelection({"ungrouped"}); QVERIFY(chips->isVisible()); QVERIFY(projects->isVisible()); }
        // The chip row shows at once and moves the list down when the layout runs,
        // which can take a while under load: sample the docked position once the
        // list sits below the laid-out row and stays there.
        int previous = -1;
        QTRY_VERIFY((!filtered || (chips->height() > 0 && top() > projects->geometry().bottom()))
                    && [&] { const int now = top(); return std::exchange(previous, now) == now; }());
        const int docked = top();
        // Each frame of the transition keeps the list where it was, also before
        // the layout requests of an animation step run.
        QList<int> frames;
        ExpansionSteps steps([&] { frames << top(); }); list->installEventFilter(&steps);
        toggle->click();
        for (int i = 0; i < 30 && list->property("expansion").toReal() > 0; ++i) { QTest::qWait(10); frames << top(); }
        QTRY_COMPARE(list->property("expansion").toReal(), 0.0); frames << top();
        // A poll rebuilds the collapsed list; at no point is it laid out lower.
        window.setFleet(fleet()); frames << top(); QTest::qWait(20); frames << top();
        QVERIFY(projects->isHidden());
        toggle->click(); QTRY_COMPARE(list->property("expansion").toReal(), 1.0); frames << top();
        for (int frame : frames) QCOMPARE(frame, docked);
    }
}

void TestSessionsWindow::escapeInActivityInterruptsAndRestoresPrompt()
{
    QTemporaryDir fixture; QFile fake(fixture.filePath("hgs")), original(script());
    QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(fake.open(QIODevice::WriteOnly));
    // Acknowledge interrupts like the CLI and record each request.
    fake.write(original.readAll().replace("if False:pass\n", "if False:pass\n"
        "elif c=='interrupt':\n payload=sys.stdin.read().rstrip('\\n')\n"
        " open(__file__+'.interrupts','a').write(payload+'\\n')\n"
        " print(json.dumps({'request_id':json.loads(payload)['request_id'],'name':a[1],'run_id':'run-one',"
        "'conversation_id':'conversation-one','status':'submitted'}))\n"));
    fake.close(); QVERIFY(FakeCli::makeRunnable(fake.fileName()));
    const auto requests = [&] { QFile log(fake.fileName() + ".interrupts"); return log.open(QIODevice::ReadOnly) ? log.readAll().count('\n') : 0; };

    SessionsWindow window(FakeCli::program(fake.fileName())); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *browser = window.findChild<ActivityView *>("mainActivity")->browser(); auto *notice = window.findChild<QLabel *>("notice");
    const QString prompt = "Fix the flaky test\nand explain the cause.";
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
        {"process_state", "running"}, {"activity", "busy"}, {"phase", "working"}, {"interrupt_supported", true},
        {"turn_started", 1790928000.0}, {"prompt", prompt}, {"events", QJsonArray{}}, {"cursor", 0}};
    const auto update = [&] { client->inspectionReady({}, "codex/hgs/dashboard", details, {}); };
    const auto interrupted = [&] { details["phase"] = "interrupted"; details["activity"] = "unknown"; details["interrupted_message_can_send"] = true; update(); };
    update(); QVERIFY(composer->findChild<QPushButton *>("interruptAgent")->isEnabled());

    // Escape in the transcript stops the turn instead of closing the window,
    // and the interrupted prompt returns to the empty message field.
    browser->setFocus(); QTest::keyClick(browser, Qt::Key_Escape);
    QTRY_COMPARE(requests(), 1); QVERIFY(window.isVisible());
    QTRY_VERIFY(notice->text().contains("Interrupt sent"));
    interrupted();
    QCOMPARE(composer->editor()->toPlainText(), prompt); QVERIFY(composer->editor()->hasFocus());

    // Escape in the message field stops the next turn; a started draft is kept.
    details["phase"] = "working"; details["activity"] = "busy"; details["turn_started"] = 1790928100.0; details.remove("interrupted_message_can_send");
    update(); setComposerText(composer, "my own draft"); composer->editor()->setFocus(); notice->clear();
    QTest::keyClick(composer->editor(), Qt::Key_Escape);
    QTRY_COMPARE(requests(), 2); QTRY_VERIFY(notice->text().contains("Interrupt sent"));
    interrupted(); QCOMPARE(composer->editor()->toPlainText(), QString("my own draft"));

    // Without a working turn, Escape only leaves the field.
    details["phase"] = "idle"; details["activity"] = "idle"; update();
    composer->editor()->setFocus(); QTest::keyClick(composer->editor(), Qt::Key_Escape);
    QVERIFY(!composer->editor()->hasFocus()); QTest::qWait(150); QCOMPARE(requests(), 2);
}

void TestSessionsWindow::stopNoticesExplainOriginalTargetAndCanBeDismissed_data()
{
    QTest::addColumn<QString>("outcome");
    for (const auto *outcome : {"inactive", "failed", "navigated"}) QTest::newRow(outcome) << QString(outcome);
}

void TestSessionsWindow::stopNoticesExplainOriginalTargetAndCanBeDismissed()
{
    QFETCH(QString, outcome);
    QTemporaryDir directory;
    const auto program = directory.filePath("hgs"), state = directory.filePath("details.json");
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"activity", "busy"}, {"phase", "working"},
        {"interrupt_supported", true}, {"turn_started", 1790928000.0}, {"prompt", "Original turn prompt"}, {"events", QJsonArray{}}, {"cursor", 0}};
    QVERIFY(FakeCli::replace(state, QJsonDocument(details).toJson()));
    QVERIFY(FakeCli::replace(directory.filePath("failure"), outcome == "failed" ? QByteArray("hgs: Native interruption could not be verified") : QByteArray("hgs: This turn is no longer working")));
    // Polls read details.json while the interrupt rewrites it: replace it, so no
    // inspection reads a truncated file and fails. Windows refuses the rename while a
    // poll has the file open; retry then.
    QFile fixture(program); QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import json,pathlib,sys,time
root=pathlib.Path(__file__).parent
if sys.argv[1]=='inspect': print((root/'details.json').read_text())
elif sys.argv[1]=='interrupt':
 p=json.load(sys.stdin)
 with (root/'calls').open('a') as f:f.write('call\n')
 d=json.loads((root/'details.json').read_text());d.update(activity='idle',phase='idle',interrupt_supported=False)
 staged=root/'details.tmp';staged.write_text(json.dumps(d))
 for _ in range(250):
  try:staged.replace(root/'details.json');break
  except PermissionError:time.sleep(.02)
 else:sys.exit('details.json stayed open')
 time.sleep(.15)
 print((root/'failure').read_text(),file=sys.stderr);sys.exit(1)
else: print('{}')
)PY"); fixture.close(); QVERIFY(FakeCli::makeRunnable(program));
    SessionsWindow window(FakeCli::program(program)); window.resize(1080, 760); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *notice = window.findChild<QLabel *>("notice"); auto *dismiss = window.findChild<QPushButton *>("dismissNotice");
    QVERIFY(dismiss); QVERIFY(dismiss->isHidden());
    auto *stop = composer->findChild<QPushButton *>("interruptAgent"); QTRY_VERIFY(stop->isEnabled());
    setComposerText(composer, "Keep my draft");
    QSignalSpy refresh(&window, &SessionsWindow::refreshRequested);
    window.showNotificationNotice("Earlier information"); stop->click();
    if (outcome == "navigated") window.showSession({}, "kimi/docs/research");
    QTRY_VERIFY(notice->text().startsWith("Stop codex/hgs/dashboard ("));
    QVERIFY(!notice->text().contains("kimi/docs/research")); QVERIFY(!refresh.isEmpty());
    QCOMPARE(notice->property("error").toBool(), outcome == "failed");
    if (outcome == "failed") {
        QVERIFY(notice->text().contains("Native interruption could not be verified"));
        QVERIFY(notice->text().contains("Refresh status or open Terminal"));
        QVERIFY(notice->toolTip().contains("Terminal"));
        // An older information timeout must not erase the newer error.
        QTest::qWait(5100); QVERIFY(notice->isVisible()); QVERIFY(dismiss->isVisible());
    } else QVERIFY(notice->text().contains("No action needed"));
    if (outcome == "navigated") window.showSession({}, "codex/hgs/dashboard");
    QTRY_VERIFY(!stop->isEnabled()); QCOMPARE(composer->editor()->toPlainText(), QString("Keep my draft"));
    QVERIFY(dismiss->isVisible()); QVERIFY(!dismiss->accessibleName().isEmpty());
    auto *statusBar = window.findChild<QFrame *>("workspaceStatus");
    QVERIFY(statusBar->rect().contains(QRect(dismiss->mapTo(statusBar, QPoint()), dismiss->size())));
    dismiss->click(); QVERIFY(notice->isHidden()); QVERIFY(dismiss->isHidden()); QVERIFY(notice->text().isEmpty()); QVERIFY(notice->toolTip().isEmpty());
    QFile calls(directory.filePath("calls")); QVERIFY(calls.open(QIODevice::ReadOnly)); QCOMPARE(calls.readAll(), QByteArray("call\n"));
}

void TestSessionsWindow::escapeInterruptsFromAnywhereAndNeverCloses()
{
    QTemporaryDir fixture; QFile fake(fixture.filePath("hgs")), original(script());
    QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write(original.readAll().replace("if False:pass\n", "if False:pass\n"
        "elif c=='interrupt':\n payload=sys.stdin.read().rstrip('\\n')\n"
        " open(__file__+'.interrupts','a').write(payload+'\\n')\n"
        " print(json.dumps({'request_id':json.loads(payload)['request_id'],'name':a[1],'run_id':'run-one',"
        "'conversation_id':'conversation-one','status':'submitted'}))\n"));
    fake.close(); QVERIFY(FakeCli::makeRunnable(fake.fileName()));
    const auto requests = [&] { QFile log(fake.fileName() + ".interrupts"); return log.open(QIODevice::ReadOnly) ? log.readAll().count('\n') : 0; };

    SessionsWindow window(FakeCli::program(fake.fileName())); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *list = window.findChild<QWidget *>("sessionList"); auto *search = window.findChild<QLineEdit *>("search");
    const QString prompt = "Rebuild the release";
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
        {"process_state", "running"}, {"activity", "busy"}, {"phase", "tool"}, {"interrupt_supported", true},
        {"turn_started", 1790928000.0}, {"prompt", prompt}, {"events", QJsonArray{}}, {"cursor", 0}};
    const auto update = [&] { client->inspectionReady({}, "codex/hgs/dashboard", details, {}); };
    update(); QVERIFY(composer->findChild<QPushButton *>("interruptAgent")->isEnabled());

    // The session list had focus after choosing the session: Escape still
    // stops the working agent and the window stays open.
    list->setFocus(); QTest::keyClick(list, Qt::Key_Escape);
    QTRY_COMPARE(requests(), 1); QVERIFY(window.isVisible());
    details["phase"] = "interrupted"; details["activity"] = "unknown"; details["interrupted_message_can_send"] = true; update();
    QCOMPARE(composer->editor()->toPlainText(), prompt);

    // Nothing is working: Escape never closes Zerus.
    details["phase"] = "idle"; details["activity"] = "idle"; details.remove("interrupted_message_can_send"); update();
    list->setFocus(); QTest::keyClick(list, Qt::Key_Escape); QTest::qWait(150);
    QVERIFY(window.isVisible()); QCOMPARE(requests(), 1);

    // A search being typed is cleared first, without stopping the agent.
    details["phase"] = "working"; details["activity"] = "busy"; details["turn_started"] = 1790928100.0; update();
    search->setFocus(); search->setText("release"); QTest::keyClick(search, Qt::Key_Escape);
    QVERIFY(search->text().isEmpty()); QTest::qWait(150); QCOMPARE(requests(), 1); QVERIFY(window.isVisible());
}

void TestSessionsWindow::suggestedMessageIsPlaceholderAndTabTakesIt()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.showSession({}, "codex/hgs/dashboard");
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    auto *editor = composer->editor(); const QString placeholder = editor->placeholderText();
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
        {"process_state", "running"}, {"activity", "idle"}, {"phase", "idle"}, {"events", QJsonArray{}}, {"cursor", 0},
        {"prompt_suggestion", QJsonObject{{"text", "run the tests"}, {"at", 1790928000.0}}}};
    const auto update = [&] { client->inspectionReady({}, "codex/hgs/dashboard", details, {}); };
    update(); QCOMPARE(editor->placeholderText(), QString("run the tests"));

    // Tab takes the suggestion into an empty field and keeps moving focus otherwise.
    editor->setPlainText("x"); editor->setFocus(); QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QString("x"));
    editor->clear(); editor->setFocus(); QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(editor->toPlainText(), QString("run the tests"));

    // A new turn makes it stale.
    editor->clear(); details["phase"] = "working"; details["activity"] = "busy"; update();
    QCOMPARE(editor->placeholderText(), placeholder);
    editor->setFocus(); QTest::keyClick(editor, Qt::Key_Tab); QVERIFY(editor->toPlainText().isEmpty());

    // Sending answers it: the same suggestion does not return before the next one.
    details["phase"] = "idle"; details["activity"] = "idle"; update();
    QCOMPARE(editor->placeholderText(), QString("run the tests"));
    auto *send = composer->findChild<QPushButton *>("sendMessage");
    editor->setPlainText("something else"); QVERIFY(send->isEnabled()); send->click();
    QCOMPARE(editor->placeholderText(), placeholder);
    update(); QCOMPARE(editor->placeholderText(), placeholder);
    details["prompt_suggestion"] = QJsonObject{{"text", "push it"}, {"at", 1790928100.0}}; update();
    QCOMPARE(editor->placeholderText(), QString("push it"));
}

void TestSessionsWindow::collapsedStripDoesNotOpenSubagents()
{
    QSettings().setValue("workspace/sessionsCollapsed", true); QSettings().setValue("workspace/expandSessionsOnHover", false);
    FleetState state; auto box = fleet().local(); auto session = box.sessions[0];
    session.subagents = {{"agent-a", QJsonObject{{"name","Review"},{"state","working"}}}, {"agent-b", QJsonObject{{"name","Tests"},{"state","finished"}}}};
    box.sessions = {session}; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(state); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *panel = window.findChild<QWidget *>("sessionListPanel"); auto *list = window.findChild<SessionList *>("sessionList");
    QTRY_COMPARE(panel->width(), SessionStrip::width(true));
    int parent = -1; for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::HasChildren).toBool()) parent = i;
    QVERIFY(parent >= 0); const int count = list->count();
    // Where a card has its agents counter, a tile only selects the session; Right opens nothing either.
    const QPoint corner = sessionCardRect(list->visualItemRect(list->item(parent))).bottomRight() - QPoint(12, 10);
    QTest::mouseMove(list->viewport(), corner); QVERIFY(list->viewport()->cursor().shape() != Qt::PointingHandCursor);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, corner);
    QCOMPARE(list->count(), count); QCOMPARE(list->currentRow(), parent);
    QTest::keyClick(list, Qt::Key_Right); QCOMPARE(list->count(), count);
    // The docked list opens active children; finished turns stay hidden.
    window.findChild<QPushButton *>("sessionPanelToggle")->click();
    QTRY_COMPARE(list->property("expansion").toReal(), 1.0);
    QTest::keyClick(list, Qt::Key_Right); QTRY_COMPARE(list->count(), count + 1);
    QCOMPARE(list->item(parent + 1)->data(SessionRoles::ChildId).toString(), QString("agent-a"));
}

void TestSessionsWindow::collapsedStripSearchOpensPanel()
{
    QSettings().setValue("workspace/sessionsCollapsed", true); QSettings().setValue("workspace/expandSessionsOnHover", false);
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window)); window.activateWindow();
    auto *panel = window.findChild<QWidget *>("sessionListPanel"); auto *search = window.findChild<QLineEdit *>("search");
    QTRY_COMPARE(panel->width(), SessionStrip::width(true));
    window.findChild<QPushButton *>("sessionStripSearch")->click();
    QTRY_VERIFY(panel->width() >= 270); QTRY_VERIFY(search->hasFocus());
    QTest::keyClick(search, Qt::Key_Escape);
    QTRY_COMPARE(panel->width(), SessionStrip::width(true));
    window.findChild<QPushButton *>("sessionStripSearch")->click(); QTRY_VERIFY(panel->width() >= 270);
    // A click on the conversation closes the panel and still reaches its target.
    struct Presses : QObject {
        int count = 0;
        bool eventFilter(QObject *, QEvent *event) override { count += event->type() == QEvent::MouseButtonPress; return false; }
    } presses;
    auto *detail = window.findChild<QStackedWidget *>("detail"); const QPoint point(detail->width() - 60, detail->height() / 2);
    QWidget *target = detail->childAt(point) ? detail->childAt(point) : detail; target->installEventFilter(&presses);
    QTest::mouseClick(target, Qt::LeftButton, {}, target->mapFrom(detail, point));
    QTRY_COMPARE(panel->width(), SessionStrip::width(true)); QCOMPARE(presses.count, 1); target->removeEventFilter(&presses);
    // Pinning the open panel docks it; the conversation then makes room once.
    window.findChild<QPushButton *>("sessionStripSearch")->click(); QTRY_VERIFY(panel->width() >= 270);
    window.findChild<QPushButton *>("sessionPanelToggle")->click();
    QTRY_COMPARE(window.findChild<QWidget *>("sessionListSlot")->width(), panel->width());
    QVERIFY(!QSettings().value("workspace/sessionsCollapsed").toBool());
}

// Screenshots of a control console (hgs.exe on Windows): this computer runs no
// sessions and manages the remote machine "mac".
void TestSessionsWindow::controlConsolePreview()
{
    const QString destination = qEnvironmentVariable("HGS_CONTROL_PREVIEW_DIR");
    if (destination.isEmpty()) QSKIP("Set HGS_CONTROL_PREVIEW_DIR to render control-console screenshots");
    QDir().mkpath(destination);
    QTemporaryDir fixture; QVERIFY(fixture.isValid()); const auto fake = fixture.filePath("hgs");
    QVERIFY(FakeCli::install(fake, "#!/usr/bin/env python3\nimport json,runpy,sys\na=sys.argv[1:]\n"
        "if a[:2]==['machine','ls']:print(json.dumps({'local':'windows-pc','revision':'r1','machines':[{'alias':'mac',"
        "'connection':{'hostname':'mac.example.test','enabled':True},'enabled':True,'source':'local','inherited':False}]}))\n"
        "elif a[:2]==['machine','resolve']:print(json.dumps({'alias':'mac','effective':{'hostname':'mac.example.test','user':'dev','port':'22','identity_files':[]}}))\n"
        "elif 'account' in a[:2]:print(json.dumps({'profiles':[]}))\n"
        "else:\n sys.argv[0]=" + FakeCli::pythonString(script()) + ";runpy.run_path(sys.argv[0],run_name='__main__')\n"));
    const auto now = QDateTime::currentMSecsSinceEpoch();
    auto state = fleet(); auto local = state.local(); const auto sessions = local.sessions;
    local.host = "windows-pc"; local.sessions.clear(); local.localSessions = false;
    local.metrics = QJsonObject{{"platform", "windows"}, {"cpu_count", 16}, {"sampled_at", now / 1000.}};
    state.setLocal(local, now);
    auto mac = *state.peer("mac"); mac.sessions.append(sessions); state.setPeer(mac, now);
    SessionsWindow window(FakeCli::program(fake)); window.resize(1280, 800); window.setFleet(state); window.show(); QTest::qWait(300);
    window.findChild<QPushButton *>("brandMark")->click(); QTest::qWait(300);
    QVERIFY(window.grab().save(destination + "/control-console-overview.png"));
    window.showSession("mac", "codex/hgs/dashboard"); QTest::qWait(500);
    QVERIFY(window.grab().save(destination + "/control-console-session.png"));
    for (auto *button : window.findChildren<QPushButton *>("railButton"))
        if (button->toolTip().startsWith("Machines")) button->click();
    QTest::qWait(1500);
    QVERIFY(window.grab().save(destination + "/control-console-machines.png"));
}

void TestSessionsWindow::preview()
{
    const QString destination = qEnvironmentVariable("HGS_PREVIEW_DIR");
    if (destination.isEmpty()) QSKIP("Set HGS_PREVIEW_DIR to render design previews");
    QDir().mkpath(destination);
    const QPalette original = qApp->palette();
    for (bool dark : {false, true}) {
        QPalette palette = original;
        palette.setColor(QPalette::Window, QColor(dark ? "#161b21" : "#f5f7f9"));
        palette.setColor(QPalette::Base, QColor(dark ? "#1c2229" : "#ffffff"));
        palette.setColor(QPalette::Text, QColor(dark ? "#e8edf4" : "#1a2733"));
        palette.setColor(QPalette::WindowText, palette.color(QPalette::Text)); qApp->setPalette(palette);
        auto previewFleet = fleet(); auto previewBox = previewFleet.local();
        auto &main = previewBox.sessions[0]; main.cwd = "/workspace/zerus-ui"; main.gitRoot = main.cwd;
        main.gitBranch = "feat/zerus"; main.gitWorktree = true; main.gitWorktreeName = "zerus-ui";
        main.activitySummary = "Bash"; main.activityDetail = "ctest --test-dir tray/build";
        main.subagentSource = "hooks"; main.subagentCountsComplete = true; main.subagentActiveCount = 6; main.subagentTotalCount = 14;
        const QStringList projects{"api", "project-docs", "project-docs", "hgs", "sample-project", "website", "api"};
        const QStringList names{"auth-flow", "dashboard", "wheel", "persistence", "research", "content", "search"};
        const QStringList commands{"claude", "codex", "codex", "codex", "kimi", "claude", "kimi"};
        const QStringList branches{"feat/auth", "feat/operations", "fix/wheel", "main", "chore/hooks", "main", "feat/search"};
        const QStringList actionTexts{"Read: Validate token expiry", "Read: Review service configuration", "apply_patch: Add keyboard navigation", "Ready: Waiting for your next message", "ReadFile: Compare deployment patterns", "Ready: Content review finished", "Shell: Check search indexes"};
        for (int i = 0; i < names.size(); ++i) {
            auto row = previewBox.sessions[0]; row.project = projects[i]; row.tag = names[i]; row.cmd = commands[i]; row.name = row.cmd + '/' + row.project + '/' + row.tag;
            row.gitBranch = branches[i]; row.gitWorktree = i == 1 || i == 2; row.gitWorktreeName = i == 1 ? "w2" : "wheel";
            row.cwd = "/workspace/" + (row.gitWorktree ? row.gitWorktreeName : row.project); row.gitRoot = row.cwd;
            row.activitySummary = actionTexts[i]; row.activityDetail.clear(); row.subagentActiveCount = 0; row.subagentTotalCount = 0;
            if (i == 3 || i == 5) { row.activity = "idle"; row.phase = "idle"; row.currentTool.clear(); }
            if (i == 3) { row.conversationId = "preview-reply"; row.replyId = "3:1790928000"; }
            if (i == 4) { row.subagentSource = "hook_profiles"; row.subagentActiveCount = 3; row.subagentTotalCount = 12; }
            if (i == 6) { row.subagentSource = "hook_profiles"; row.subagentTotalCount = -1; row.subagentCountsComplete = false; }
            previewBox.sessions.append(row);
        }
        previewFleet.setLocal(previewBox, QDateTime::currentMSecsSinceEpoch());
        SessionOrganization previewOrg;
        const auto releaseGroup = previewOrg.createGroup("Release / Zerus"), infraGroup = previewOrg.createGroup("Infrastructure"), experimentsGroup = previewOrg.createGroup("Research & experiments");
        previewOrg.setColor(releaseGroup,"#4ba3ff"); previewOrg.setColor(infraGroup,"#ffcc00"); previewOrg.setColor(experimentsGroup,"#bb86fc");
        previewOrg.addFolder(releaseGroup,"arch","/workspace/zerus","Linux checkout");
        previewOrg.addFolder(releaseGroup,"mac","/Users/example/work/hgs","Mac checkout");
        for (const auto &row : previewBox.sessions) {
            const auto id = previewOrg.observe("arch", row.name, "");
            previewOrg.moveSession(id, row.project == "hgs" ? releaseGroup : row.project == "sample-project" ? infraGroup : experimentsGroup);
        }
        previewOrg.moveSession(previewOrg.observe("mac", "claude/infra/review", ""), infraGroup); previewOrg.setCollapsed(infraGroup, true);
        QSettings().setValue("workspace/organization", QJsonDocument(previewOrg.toJson()).toJson(QJsonDocument::Compact));
        SessionsWindow window(program()); window.setFleet(previewFleet); window.resize(1240, 800); window.show(); QTest::qWait(100);
        auto *pin = window.findChild<QPushButton *>("windowPin");
        for (bool pinned : {false, true}) {
            pin->setChecked(pinned);
            QTest::mouseMove(&window, QPoint(window.width() - 10, 10)); QTest::qWait(30);
            const auto name = QString("/window-%1-%2").arg(pinned ? "pinned" : "unpinned", dark ? "dark" : "light");
            const auto crop = pin->geometry().adjusted(-7, -8, 7, 55);
            QVERIFY(pin->parentWidget()->grab(crop).save(destination + name + ".png"));
            QTest::mouseMove(pin, pin->rect().center()); QTest::qWait(30);
            QVERIFY(pin->parentWidget()->grab(crop).save(destination + name + "-hover.png"));
        }
        pin->setChecked(false);
        window.showSession({}, "codex/hgs/dashboard"); QTest::qWait(100);
        auto *client = window.findChild<HgsClient *>();
        QJsonObject data{{"tracked", true}, {"conversation_id", "conversation-one"}, {"phase", "tool"}, {"activity", "busy"},
            {"cwd", "/workspace/zerus"}, {"model", "gpt-6.1-sol"}, {"prompt", "Build a clear view of every agent across our machines.\nKeep the interface quiet, with attention where it matters."}, {"cursor", 3},
            {"events", QJsonArray{
                QJsonObject{{"seq", 1}, {"at", 1790927810}, {"type", "UserPromptSubmit"}, {"detail", "Build the session dashboard."}},
                QJsonObject{{"seq", 2}, {"at", 1790927990}, {"type", "PostToolUse"}, {"tool", "apply_patch"}, {"detail", "Updated tray/src/SessionsWindow.cpp"}},
                QJsonObject{{"seq", 3}, {"at", 1790928000}, {"type", "PreToolUse"}, {"tool", "Bash"}, {"detail", "ctest --test-dir tray/build --output-on-failure"}}}}};
        QJsonObject children;
        const QStringList childNames{"UI layout", "Session state", "macOS parity", "Search", "Git metadata", "Accessibility", "Concurrency", "Hooks", "Archive", "Projects", "Packaging", "Tests", "Docs", "Terminal"};
        const QStringList childActions{"Check compact row geometry", "Verify saved context recovery", "Build native menu integration", "Test branch and label filters", "Read linked worktree roots", "Check keyboard focus order", "Review lock ordering", "Inspect activity event contract", "Test archive identity", "Verify remote folder browser", "Check release bundle", "Run focused regression tests", "Update session examples", "Verify attached title changes"};
        for (int i = 0; i < childNames.size(); ++i) children[QString("child-%1").arg(i)] = QJsonObject{{"label", childNames[i]}, {"state", i < 6 ? "working" : "finished"}, {"detail", childActions[i]}, {"updated", 1790928000 - i}};
        data["task_lists"] = QJsonObject{{"main", QJsonObject{{"items", QJsonArray{
            QJsonObject{{"title", "Inspect the workspace and existing hooks"}, {"status", "completed"}},
            QJsonObject{{"title", "Build the collapsible session inspector"}, {"status", "in_progress"}},
            QJsonObject{{"title", "Verify native behavior on Arch and macOS"}, {"status", "pending"}}
        }}}}};
        data["goal"] = QJsonObject{{"id", "preview-goal"}, {"objective", "Complete the migration and verify restore on both machines"},
            {"status", "active"}, {"tokens_used", 24000}, {"token_budget", 100000}, {"time_used_seconds", 154800}};
        data["subagents"] = children; data["subagent_source"] = "hooks"; data["subagent_active_count"] = 6; data["subagent_total_count"] = 14;
        data["cwd"] = "/workspace/zerus-ui"; data["git_root"] = "/workspace/zerus-ui"; data["git_branch"] = "feat/zerus";
        data["git_worktree"] = true; data["git_worktree_name"] = "zerus-ui";
        data["effort"] = "high"; data["settings_change_supported"] = true; data["settings_apply_when"] = "ready";
        data["model_options"] = QJsonArray{
            QJsonObject{{"id", "gpt-6.1-sol"}, {"effort_options", QJsonArray{"low", "medium", "high", "xhigh"}}},
            QJsonObject{{"id", "gpt-6-astra"}, {"effort_options", QJsonArray{"high", "xhigh", "ultra"}}}};
        client->inspectionReady({}, "codex/hgs/dashboard", data); QTest::qWait(50);
        auto *tree = window.findChild<SessionList *>("sessionList");
        for (int i = 0; i < tree->count(); ++i) if (tree->item(i)->data(SessionRoles::HasChildren).toBool()) { tree->childrenToggled(tree->item(i)->data(SessionRoles::Key).toString()); break; }
        QTest::qWait(30);
        auto *usage=static_cast<AccountUsage::Button *>(window.findChild<QPushButton *>("sessionAccountUsage"));
        usage->setData(QJsonObject{{"provider","codex"},{"status","ok"},{"windows",QJsonArray{
            QJsonObject{{"used_percent",78},{"window_minutes",300}},QJsonObject{{"used_percent",94},{"window_minutes",10080}}}}});
        QVERIFY(window.grab().save(destination + (dark ? "/tree-dark.png" : "/tree-light.png")));
        for (int i = 0; i < tree->count(); ++i) if (tree->item(i)->data(SessionRoles::HasChildren).toBool()) { tree->childrenToggled(tree->item(i)->data(SessionRoles::Key).toString()); break; }
        auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
        auto *inspectorToggle = window.findChild<QPushButton *>("toggleInspector");
        inspectorToggle->setChecked(true); QTest::qWait(50);
        QVERIFY(window.grab().save(destination + (dark ? "/swarm-dark.png" : "/swarm-light.png")));
        inspectorToggle->setChecked(false); QTest::qWait(30);
        QVERIFY(window.grab().save(destination + (dark ? "/sessions-dark.png" : "/sessions-light.png")));
        window.showNotificationNotice("Session updated. Refreshing…");
        QVERIFY(window.grab().save(destination + (dark ? "/status-dark.png" : "/status-light.png")));
        window.findChild<MessageComposer *>("messageComposer")->findChild<QPushButton *>("sessionModelSettings")->click(); QTest::qWait(30);
        auto *modelPopup = window.findChild<MessageComposer *>("messageComposer")->findChild<QWidget *>("sessionSettingsPopup");
        QVERIFY(modelPopup->grab().save(destination + (dark ? "/model-settings-dark.png" : "/model-settings-light.png"))); modelPopup->hide();
        auto *sessionList = window.findChild<QListWidget *>("sessionList");
        QTest::keyClick(sessionList, Qt::Key_Menu); QTest::qWait(50);
        auto *contextMenu = window.findChild<QMenu *>("sessionContextMenu");
        QVERIFY(contextMenu->grab().save(destination + (dark ? "/session-menu-dark.png" : "/session-menu-light.png")));
        contextMenu->hide();
        auto *groupsMenu = window.findChild<QMenu *>("sessionProjectsMenu");
        groupsMenu->popup(sessionList->mapToGlobal(QPoint(8, 8))); QTest::qWait(30);
        groupsMenu->setActiveAction(window.findChild<QAction *>("markAllSessionsRead"));
        QVERIFY(groupsMenu->grab().save(destination + (dark ? "/mark-all-read-dark.png" : "/mark-all-read-light.png"))); groupsMenu->hide();
        QTimer::singleShot(0, &window, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
            QTimer::singleShot(1000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("renameSessionName")->setText("release planning");
            QTest::qWait(50);
            QVERIFY(dialog->grab().save(destination + (dark ? "/rename-dark.png" : "/rename-light.png")));
            dialog->reject();
        });
        window.findChild<QAction *>("renameSessionAction")->trigger();
        auto *actions = window.findChild<QPushButton *>("batchActions");
        auto *menu = actions->menu(); menu->popup(actions->mapToGlobal(QPoint(0, actions->height())));
        QTest::qWait(50); menu->setActiveAction(menu->actions().first());
        QVERIFY(menu->grab().save(destination + (dark ? "/actions-dark.png" : "/actions-light.png")));
        auto *machineMenu = menu->actions().first()->menu();
        machineMenu->popup(menu->mapToGlobal(menu->actionGeometry(menu->actions().first()).topRight())); QTest::qWait(50);
        QVERIFY(machineMenu->grab().save(destination + (dark ? "/machine-actions-dark.png" : "/machine-actions-light.png")));
        machineMenu->hide(); menu->hide();
        window.resize(960, 650); QTest::qWait(50);
        QVERIFY(window.grab().save(destination + (dark ? "/sessions-dark-compact.png" : "/sessions-light-compact.png")));
        inspectorToggle->setChecked(true); QTest::qWait(50);
        QVERIFY(window.grab().save(destination + (dark ? "/swarm-dark-compact.png" : "/swarm-light-compact.png")));
        inspectorToggle->setChecked(false);
        window.showSession("mac", "claude/infra/review"); QTest::qWait(50);
        QVERIFY(window.grab().save(destination + (dark ? "/attention-dark.png" : "/attention-light.png")));
        auto archiveFleet = fleet(); auto archiveBox = archiveFleet.local();
        auto archived = archiveBox.sessions[0]; archived.name = "codex/website/navigation";
        archived.project = "website"; archived.tag = "navigation"; archived.state = "archived";
        archived.archiveId = "preview-archive"; archived.archivedAt = 1790928000;
        archived.currentTool.clear(); archived.prompt = "Simplify the mobile navigation and document the final design.";
        // The older completed instance has its own identity; no active binding occupies its name.
        archiveBox.sessions.removeAt(2); archiveBox.sessions.append(archived);
        archiveFleet.setLocal(archiveBox, QDateTime::currentMSecsSinceEpoch()); window.setFleet(archiveFleet);
        window.resize(1240, 800);
        for (auto *button : window.findChildren<QPushButton *>()) if (button->property("filter") == "archived") { button->click(); button->setFocus(); }
        QTest::qWait(60);
        auto archiveDetails = data; archiveDetails["prompt"] = archived.prompt; archiveDetails["phase"] = "ended";
        archiveDetails["activity"] = "ended"; archiveDetails["cwd"] = "/workspace/projects/website";
        archiveDetails["last_message"] = "Navigation updated. The mobile menu now preserves focus and the design notes are in place.";
        archiveDetails["completion_reason"] = "clean_exit";
        client->inspectionReady({}, archived.name, archiveDetails, archived.archiveId); QTest::qWait(50);
        QVERIFY(window.grab().save(destination + (dark ? "/archive-dark.png" : "/archive-light.png")));
        window.setFleet(fleet()); window.showSession({}, "codex/hgs/dashboard");
        NewSessionDialog launch(program(), fleet(), "mac", "codex", {}, &window); launch.show(); QTest::qWait(100);
        SessionOrganization launchProjects; const auto launchId=launchProjects.createGroup("Zerus");
        launchProjects.addFolder(launchId,"mac","/Users/example/work/new project"); launchProjects.addFolder(launchId,"arch","/workspace/zerus");
        launch.setGroups(launchProjects,launchId);
        QVERIFY(launch.grab().save(destination + (dark ? "/new-session-dark.png" : "/new-session-light.png"))); launch.close();
        DirectoryDialog browser(program(), "mac", "~", &window); browser.show(); QTest::qWait(100);
        QVERIFY(browser.grab().save(destination + (dark ? "/folders-dark.png" : "/folders-light.png"))); browser.close();
        window.resize(1240, 800); window.showProjects("mac"); QTest::qWait(100);
        QVERIFY(window.grab().save(destination + (dark ? "/projects-dark.png" : "/projects-light.png")));
        // Compare selection, activity, unread and action states at the smallest
        // supported sidebar width, using the real delegate in both densities.
        QSettings().remove("workspace/organization");
        BoxState states; states.host = "arch"; states.ok = true;
        const QStringList stateNames{"nebula", "infrastructure-rollout-with-a-long-name", "persistence", "deploy", "research", "tests", "navigation", "content", "legacy"};
        for (int i = 0; i < stateNames.size(); ++i) {
            auto row = fleet().local().sessions[0]; row.tag = stateNames[i]; row.name = "codex/hgs/" + row.tag;
            row.gitBranch = "main"; row.activitySummary = "Bash"; row.activityDetail = "Run the deployment checks";
            row.model = "gpt-6-astra"; row.effort = "xhigh";
            if (i == 0) {
                row.turnStarted = QDateTime::currentSecsSinceEpoch() - 2729;
                row.subagentSource = "hooks"; row.subagentCountsComplete = true;
                row.subagentActiveCount = 2; row.subagentTotalCount = 3;
                row.subagents = {{"review", QJsonObject{{"name", "Review"}, {"state", "working"}}}};
            }
            if (i == 1 || i == 2) { row.conversationId = "preview-" + row.tag; row.replyId = "1:1790928000"; }
            if (i == 2 || i >= 6) { row.activity = "idle"; row.phase = "idle"; row.activitySummary.clear(); row.activityDetail.clear(); row.currentTool.clear(); row.prompt.clear(); }
            if (i == 3) { row.phase = "approval"; row.activityDetail = "Approve the deployment command"; }
            if (i == 4) { row.phase = "input"; row.activityDetail = "Choose the target environment"; }
            if (i == 5) { row.phase = "error"; row.activityDetail = "Connection timed out"; }
            if (i == 6) row.state = "paused";
            if (i == 8) { row.tracked = false; row.activity.clear(); row.phase.clear(); }
            states.sessions.append(row);
        }
        FleetState stateFleet; stateFleet.setLocal(states, QDateTime::currentMSecsSinceEpoch());
        SessionsWindow stateWindow(program()); stateWindow.setFleet(stateFleet); stateWindow.resize(1000, 1180); stateWindow.show();
        stateWindow.showSession({}, states.sessions[0].name);
        auto *stateList = stateWindow.findChild<SessionList *>("sessionList");
        auto *railSplitter = qobject_cast<QSplitter *>(stateWindow.findChild<QWidget *>("sessionListSlot")->parentWidget());
        QVERIFY(railSplitter);
        for (const int railWidth : {270, 380}) for (bool compact : {true, false}) {
            railSplitter->setSizes({railWidth, railSplitter->width() - railSplitter->handleWidth() - railWidth});
            stateList->setProperty("compact", compact); stateList->doItemsLayout(); QTest::qWait(60);
            QPixmap rendered(stateList->size() * stateList->devicePixelRatioF()); rendered.setDevicePixelRatio(stateList->devicePixelRatioF());
            rendered.fill(QColor(dark ? "#161b21" : "#f5f7f9")); stateList->render(&rendered);
            QVERIFY(rendered.save(destination + QString("/states-%1-%2%3.png").arg(dark ? "dark" : "light", compact ? "compact" : "comfortable", railWidth == 270 ? "" : "-wide")));
        }
        for (const auto *pageName : {"dashboardPage", "machinesPage", "accountsPage"}) {
            auto *page = stateWindow.findChild<QWidget *>(pageName); QVERIFY(page);
            auto *stack = qobject_cast<QStackedWidget *>(page->parentWidget()); QVERIFY(stack);
            stack->setCurrentWidget(page); QTest::qWait(30);
            QVERIFY(page->grab(QRect(0, 0, page->width(), 90)).save(destination + QString("/header-%1-%2.png").arg(dark ? "dark" : "light", pageName)));
        }
    }
    qApp->setPalette(original);
}

void TestSessionsWindow::launchInitialFolderFollowsProject_data()
{
    QTest::addColumn<bool>("hasFolder");
    QTest::newRow("project-folder")<<true;
    QTest::newRow("empty-project")<<false;
}
void TestSessionsWindow::launchInitialFolderFollowsProject()
{
    QFETCH(bool,hasFolder);
    SessionOrganization projects;
    const auto first=projects.createGroup("Default"),target=projects.createGroup("Selected");
    projects.setDefaultProject(first);
    projects.addFolder(first,"arch","/default/unrelated");
    const auto expected=hasFolder?QString("/selected/work"):QString();
    if(hasFolder)projects.addFolder(target,"arch",expected);
    projects.moveSession("arch\ncodex/hgs/dashboard",target);
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));

    // Callers may supply a fresher organization after construction. An automatic
    // constructor default is not a folder explicitly chosen by the user.
    NewSessionDialog dialog(program(),fleet());
    dialog.setGroups(projects,target);
    QCOMPARE(dialog.findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString(),expected);
    QVERIFY(!dialog.findChild<QLabel *>("muted")->property("outsideProject").toBool());

    // The global action derives the project from the selected session.
    SessionsWindow window(program());window.setFleet(fleet());window.show();
    window.showSession({},"codex/hgs/dashboard");
    bool checked=false;
    QTimer::singleShot(0,&window,[&]{
        auto *launch=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());
        if(!launch)return;
        const auto actualProject=launch->findChild<QComboBox *>("launchProject")->currentData().toString();
        const auto actualPath=launch->findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString();
        const bool outside=launch->findChild<QLabel *>("muted")->property("outsideProject").toBool();
        launch->reject();checked=true;
        QCOMPARE(actualProject,target);QCOMPARE(actualPath,expected);QVERIFY(!outside);
    });
    window.showNewSession();QVERIFY(checked);
}

void TestSessionsWindow::launchFolderFollowsSelectedSession_data()
{
    QTest::addColumn<QString>("machine");QTest::addColumn<QString>("cwd");
    QTest::addColumn<QString>("canonical");QTest::addColumn<QString>("expected");
    QTest::newRow("ordinary-folder")<<QString("arch")<<QString("/notes")<<QString()<<QString("/notes");
    QTest::newRow("nested-folder")<<QString("arch")<<QString("/notes/drafts")<<QString()<<QString("/notes");
    QTest::newRow("path-boundary")<<QString("arch")<<QString("/notes-copy")<<QString()<<QString("/notes-copy");
    QTest::newRow("canonical-alias")<<QString("arch")<<QString("/alias/drafts")<<QString("/notes/drafts")<<QString("/notes");
    QTest::newRow("remote-folder")<<QString("mac")<<QString("/notes/drafts")<<QString()<<QString("/notes");
    QTest::newRow("main-checkout")<<QString("arch")<<QString("/repo/src")<<QString()<<QString("/repo");
    QTest::newRow("linked-worktree")<<QString("arch")<<QString("/linked/src")<<QString()<<QString("/repo");
    QTest::newRow("remote-linked-worktree")<<QString("mac")<<QString("/linked/src")<<QString()<<QString("/repo");
}
void TestSessionsWindow::launchFolderFollowsSelectedSession()
{
    QFETCH(QString,machine);QFETCH(QString,cwd);QFETCH(QString,canonical);QFETCH(QString,expected);
    QTemporaryDir fixture;QFile program(fixture.filePath("hgs"));QFile original(script());
    QVERIFY(original.open(QIODevice::ReadOnly));QVERIFY(program.open(QIODevice::WriteOnly));program.write(original.readAll());program.close();
    QVERIFY(FakeCli::makeRunnable(program.fileName()));
    QFile response(program.fileName()+".worktrees");QVERIFY(response.open(QIODevice::WriteOnly));
    response.write(QJsonDocument(QJsonObject{{"state","ok"},{"selected_root",cwd.startsWith("/linked/")?"/linked":"/repo"},
        {"common_dir","/repo/.git"},{"sampled_at",QDateTime::currentSecsSinceEpoch()},
        {"worktrees",QJsonArray{QJsonObject{{"path","/repo"},{"kind","main"},{"available",true}},
            QJsonObject{{"path","/linked"},{"kind","linked"},{"available",true}}}}}).toJson());response.close();
    SessionOrganization projects;const auto project=projects.createGroup("Selected"),other=projects.createGroup("Other");
    projects.addFolder(project,machine,"/unrelated");projects.addFolder(project,machine,"/repo");
    projects.addFolder(project,machine,"/notes");projects.addFolder(other,machine,"/elsewhere");
    projects.moveSession("arch\nkimi/docs/research",other);
    // Same paths on another computer must never supply the launch target.
    projects.addFolder(project,machine=="mac"?"arch":"mac","/notes");
    auto state=fleet();const QString target=machine=="arch"?QString():machine;
    auto box=target.isEmpty()?state.local():*state.peer(target);auto &session=box.sessions[0];
    session.cwd=cwd;session.canonicalCwd=canonical;
    projects.moveSession(machine+'\n'+session.name,project);
    if(target.isEmpty())state.setLocal(box,QDateTime::currentMSecsSinceEpoch());else state.setPeer(box,QDateTime::currentMSecsSinceEpoch());
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(FakeCli::program(program.fileName()));window.setFleet(state);window.show();window.showSession(target,session.name);
    bool checked=false;
    QTimer::singleShot(0,&window,[&]{
        auto *dialog=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
        QTimer::singleShot(5000,dialog,&QDialog::reject);
        QCOMPARE(dialog->findChild<QComboBox *>("launchComputer")->currentData().toString(),target);
        QCOMPARE(dialog->findChild<QComboBox *>("launchProject")->currentData().toString(),project);
        QTRY_COMPARE(dialog->findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString(),expected);
        QTRY_VERIFY(!dialog->findChild<QLabel *>("muted")->text().contains("Checking"));
        checked=true;dialog->reject();
    });
    window.findChild<QPushButton *>("newSession")->click();QVERIFY(checked);
    // Explicit launch paths keep linked worktrees; inheritance applies only to +.
    QTimer::singleShot(0,&window,[&]{
        auto *dialog=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString(),QString("/linked"));
        QVERIFY(dialog->findChild<QPushButton *>("launchWithoutWorktree")->isChecked());dialog->reject();
    });
    window.showNewSession({},project,target,{},"/linked");
    auto *list=window.findChild<SessionList *>("sessionList");QVERIFY(list);
    // Selecting a project heading leaves the previous conversation open. It
    // must not inherit that conversation's folder into the heading's project.
    for(int row=0;row<list->count();++row)if(list->item(row)->data(SessionRoles::Header).toBool()
        &&list->item(row)->data(SessionRoles::Group).toString()==other){list->setCurrentRow(row);break;}
    QTimer::singleShot(0,&window,[&]{
        auto *dialog=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString(),QString("/elsewhere"));dialog->reject();
    });
    window.showNewSession({}, {},target);
}
void TestSessionsWindow::launchFolderPrefillPreservesUserChoices_data()
{
    QTest::addColumn<QString>("choice");
    for(const auto *choice:{"folder","same-folder","project","machine","path","fleet-refresh"})QTest::newRow(choice)<<QString(choice);
}
void TestSessionsWindow::launchFolderPrefillPreservesUserChoices()
{
    QFETCH(QString,choice);
    SessionOrganization projects;const auto project=projects.createGroup("First"),other=projects.createGroup("Other");
    const auto linked=projects.addFolder(project,"arch","/linked"),alternate=projects.addFolder(project,"arch","/alternate");
    projects.addFolder(project,"arch","/repo");projects.addFolder(project,"mac","/remote");projects.addFolder(other,"arch","/other");
    NewSessionDialog dialog(program(),fleet(),{},"codex",project);dialog.setGroups(projects,project);dialog.prefillSessionFolder("/linked");
    auto *folder=dialog.findChild<QComboBox *>("launchProjectFolder");auto *machine=dialog.findChild<QComboBox *>("launchComputer");
    auto *client=dialog.findChild<HgsClient *>();QVERIFY(client);
    QString expected=choice=="fleet-refresh"?"/repo":"/linked";
    if(choice=="folder"){folder->setCurrentIndex(folder->findData(alternate));expected="/alternate";}
    if(choice=="same-folder")folder->activated(folder->findData(linked));
    if(choice=="project"){auto *projectBox=dialog.findChild<QComboBox *>("launchProject");projectBox->setCurrentIndex(projectBox->findData(other));expected="/other";}
    if(choice=="machine"){machine->setCurrentIndex(machine->findData("mac"));expected="/remote";}
    if(choice=="path"){dialog.selectPath("/custom");expected="/custom";}
    if(choice=="fleet-refresh")dialog.setFleet(fleet());
    const QJsonObject catalog{{"state","ok"},{"selected_root","/linked"},{"common_dir","/repo/.git"},
        {"worktrees",QJsonArray{QJsonObject{{"path","/repo"},{"kind","main"},{"available",true}},QJsonObject{{"path","/linked"},{"kind","linked"},{"available",true}}}}};
    // Inject all outstanding IDs before the scripted process finishes. Only the
    // exact inherited request may change the folder, and only until user choice.
    for(quint64 id=1;id<=8;++id)client->worktreesReady(id,{},"/linked",catalog);
    QCOMPARE(folder->currentData(Qt::UserRole+2).toString(),expected);
}
void TestSessionsWindow::launchFolderPrefillRejectsUnverifiedMain_data()
{
    QTest::addColumn<QString>("problem");
    for(const auto *problem:{"stale","partial","foreign-root","foreign-host","unavailable","bare","ambiguous","failed"})QTest::newRow(problem)<<QString(problem);
}
void TestSessionsWindow::launchFolderPrefillRejectsUnverifiedMain()
{
    QFETCH(QString,problem);
    SessionOrganization projects;const auto project=projects.createGroup("First");projects.addFolder(project,"arch","/linked");projects.addFolder(project,"arch","/repo");
    NewSessionDialog dialog(program(),fleet(),{},"codex",project);dialog.setGroups(projects,project);dialog.prefillSessionFolder("/linked/sub");
    auto *client=dialog.findChild<HgsClient *>();QVERIFY(client);
    QJsonObject main{{"path","/repo"},{"kind","main"},{"available",true}};
    if(problem=="unavailable")main["available"]=false;
    if(problem=="bare")main["kind"]="bare";
    QJsonArray trees{main,QJsonObject{{"path","/linked"},{"kind","linked"},{"available",true}}};
    if(problem=="ambiguous")trees.append(main);
    QJsonObject catalog{{"state","ok"},{"selected_root",problem=="foreign-root"?"/linked-other":"/linked"},{"common_dir","/repo/.git"},{"worktrees",trees}};
    if(problem=="stale"||problem=="partial")catalog[problem]=true;
    for(quint64 id=1;id<=8;++id){
        if(problem=="failed")client->worktreesFailed(id,{},"/linked/sub","Unavailable");
        else client->worktreesReady(id,problem=="foreign-host"?"mac":QString(),"/linked/sub",catalog);
    }
    QCOMPARE(dialog.findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString(),QString("/linked"));
}

void TestSessionsWindow::launchFoldersFollowProject()
{
    SessionOrganization projects;const auto first=projects.createGroup("First"),second=projects.createGroup("Second"),empty=projects.createGroup("Empty");
    const auto original=projects.addFolder(first,"mac","/remote/chosen");
    const auto alternative=projects.addFolder(first,"mac","/remote/list-choice");
    projects.addFolder(second,"mac","/remote/different");projects.addFolder(second,"arch","/local/second");
    NewSessionDialog dialog(program(),fleet(),"mac","codex");dialog.setGroups(projects,first);QVERIFY(dialog.selectFolder(original));dialog.show();
    auto *project=dialog.findChild<QComboBox *>("launchProject");auto *machine=dialog.findChild<QComboBox *>("launchComputer");auto *folder=dialog.findChild<QComboBox *>("launchProjectFolder");auto *account=dialog.findChild<QListWidget *>("launchAccount");
    auto *hint=dialog.findChild<QLabel *>("muted");QVERIFY(hint);
    QTRY_VERIFY(account->currentItem()&&account->currentItem()->data(Qt::UserRole).toString()=="native-codex");
    auto *agent=dialog.findChild<QListWidget *>("launchAccount");QTest::qWait(1);const auto agentTop=agent->geometry().top();
    auto change=[&](const QString &id){project->setCurrentIndex(project->findData(id));};
    folder->setCurrentIndex(folder->findData(alternative));dialog.setFleet(fleet());
    QCOMPARE(folder->currentData(Qt::UserRole+2).toString(),QString("/remote/list-choice"));
    change(second);QCOMPARE(machine->currentData().toString(),QString("mac"));QCOMPARE(folder->currentData(Qt::UserRole+2).toString(),QString("/remote/different"));
    QVERIFY(!hint->property("outsideProject").toBool());QCOMPARE(account->currentItem()->data(Qt::UserRole).toString(),QString("native-codex"));
    change(empty);QVERIFY(folder->currentData(Qt::UserRole+2).toString().isEmpty());
    QTest::qWait(1);QCOMPARE(agent->geometry().top(),agentTop);
    dialog.setFleet(fleet());QVERIFY(folder->currentData(Qt::UserRole+2).toString().isEmpty());
    change(first);QCOMPARE(folder->currentData().toString(),original);QVERIFY(!hint->property("outsideProject").toBool());QVERIFY(hint->styleSheet().isEmpty());
    dialog.selectPath("/remote/contextual");dialog.setFleet(fleet());QCOMPARE(folder->currentData(Qt::UserRole+2).toString(),QString("/remote/contextual"));
    QTest::qWait(1);QCOMPARE(agent->geometry().top(),agentTop);
    auto *worktrees=dialog.findChild<QAction *>("chooseLaunchWorktree");
    auto *newWorktree=dialog.findChild<QAction *>("newLaunchWorktree");
    worktrees->setVisible(true);newWorktree->setVisible(true);QTest::qWait(1);QCOMPARE(agent->geometry().top(),agentTop);
    worktrees->setVisible(false);newWorktree->setVisible(false);QTest::qWait(1);QCOMPARE(agent->geometry().top(),agentTop);
    change(second);QCOMPARE(folder->currentData(Qt::UserRole+2).toString(),QString("/remote/different"));
    machine->setCurrentIndex(machine->findData(QString()));QCOMPARE(folder->currentData(Qt::UserRole+2).toString(),QString("/local/second"));
    change(empty);QCOMPARE(machine->currentData().toString(),QString());QVERIFY(folder->currentData(Qt::UserRole+2).toString().isEmpty());
    machine->setCurrentIndex(machine->findData("mac"));QVERIFY(folder->currentData(Qt::UserRole+2).toString().isEmpty());
    const auto offline=projects.addFolder(first,"missing","/offline/project");dialog.setGroups(projects,first);QVERIFY(dialog.selectFolder(offline));change(empty);
    QCOMPARE(machine->currentData().toString(),QString("missing"));QVERIFY(folder->currentData(Qt::UserRole+2).toString().isEmpty());QVERIFY(!dialog.findChild<QPushButton *>("primary")->isEnabled());
}

void TestSessionsWindow::launchListsSurviveFleetRefreshes()
{
    // Polls arrive while a list is open; rebuilding its rows would reset the hovered row.
    SessionOrganization projects;const auto id=projects.createGroup("First");
    projects.addFolder(id,"mac","/remote/one");const auto two=projects.addFolder(id,"mac","/remote/two");
    NewSessionDialog dialog(program(),fleet(),"mac","codex");dialog.setGroups(projects,id);QVERIFY(dialog.selectFolder(two));dialog.show();
    auto *folder=dialog.findChild<QComboBox *>("launchProjectFolder");auto *machine=dialog.findChild<QComboBox *>("launchComputer");
    QSignalSpy folderRows(folder->model(),&QAbstractItemModel::rowsRemoved),machineRows(machine->model(),&QAbstractItemModel::rowsRemoved);
    dialog.setFleet(fleet());
    QCOMPARE(folderRows.count(),0);QCOMPARE(machineRows.count(),0);QCOMPARE(folder->currentData().toString(),two);
    QVERIFY(folder->currentData(Qt::UserRole+3).toBool());QVERIFY(dialog.findChild<QPushButton *>("browseLaunchFolder")->isEnabled());
    // Availability still follows the fleet, in place.
    auto offline=fleet();BoxState mac;mac.host="mac";mac.ok=false;offline.setPeer(mac,QDateTime::currentMSecsSinceEpoch());
    dialog.setFleet(offline);
    QCOMPARE(folderRows.count(),0);QCOMPARE(machineRows.count(),0);QCOMPARE(folder->currentData().toString(),two);
    QVERIFY(!folder->currentData(Qt::UserRole+3).toBool());QVERIFY(machine->currentText().endsWith(" (offline)"));
    QVERIFY(!dialog.findChild<QPushButton *>("primary")->isEnabled());
    dialog.setFleet(fleet());QVERIFY(folder->currentData(Qt::UserRole+3).toBool());QCOMPARE(machine->currentText(),QString("mac"));
    // A changed folder list is still shown.
    projects.addFolder(id,"mac","/remote/three");dialog.setGroups(projects,id);
    QVERIFY(folder->findData("/remote/three",Qt::UserRole+2)>=0);
}

void TestSessionsWindow::sessionPanelHeaderStartsNewSession()
{
    QSettings().setValue("workspace/expandSessionsOnHover", false);
    SessionsWindow window(program()); window.resize(1280, 860); window.setFleet(fleet()); window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *create = window.findChild<QPushButton *>("sessionPanelNewSession"), *toggle = window.findChild<QPushButton *>("sessionPanelToggle");
    QVERIFY(create && toggle); QVERIFY(create->isVisible()); QCOMPARE(create->parentWidget()->objectName(), QString("sessionListPanel"));
    QCOMPARE(create->property("glyph").toString(), QString("add")); QVERIFY(create->x() < toggle->x());
    bool opened = false;
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());
        opened = dialog != nullptr; if (dialog) dialog->reject();
    });
    create->click(); QVERIFY(opened);
    // The strip keeps only its toggle; the rail still offers New session.
    toggle->click(); QTRY_COMPARE(window.findChild<SessionList *>("sessionList")->property("expansion").toReal(), 0.0);
    QVERIFY(!create->isVisible());
    toggle->click(); QTRY_VERIFY(create->isVisible());
}

void TestSessionsWindow::launchAddsOnlyFoldersOutsideTheProject()
{
    // A worktree of a project folder is reached through that folder; only an
    // outside folder joins the project once its launch is observed.
    SessionOrganization organization;const auto group=organization.createGroup("Design");
    organization.addFolder(group,"mac","/repo");
    QSettings().setValue("workspace/organization",QJsonDocument(organization.toJson()).toJson(QJsonDocument::Compact));
    auto state=fleet();auto box=*state.peer("mac");
    SessionsWindow window(program());window.setFleet(state);window.show();
    QSignalSpy launches(&window,&SessionsWindow::newSessionRequested);QStringList paths;
    const auto start=[&](const QString &path,const QString &name,const QString &hint){
        QTimer::singleShot(0,&window,[&,name,hint]{
            auto *dialog=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
            // The safety reject outlasts the waits: slow Windows runners start each fake CLI through Python.
            QTimer::singleShot(20000,dialog,&QDialog::reject);
            QTRY_COMPARE_WITH_TIMEOUT(dialog->findChild<QLabel *>("muted")->text(),hint,15000);
            dialog->findChild<QPushButton *>("launchWithoutWorktree")->click();
            dialog->findChild<QCheckBox *>("launchOpenTerminal")->setChecked(true);
            dialog->findChild<QLineEdit *>("launchName")->setText(name);
            auto *button=dialog->findChild<QPushButton *>("primary");QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(),15000);button->click();
        });
        window.showNewSession("codex",group,"mac",{},path);QVERIFY(!launches.isEmpty());
        SessionInfo created;created.name="codex/infra/"+name;created.cmd="codex";created.project="infra";created.tag=name;
        created.runId="run-"+name;created.state="running";created.launchId=launches.last()[5].toString();
        box.sessions.append(created);state.setPeer(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
        const SessionOrganization stored(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
        QCOMPARE(stored.groupFor("mac\ncodex/infra/"+name),group);
        paths.clear();for(const auto &folder:stored.group(group)->folders)paths<<folder.path;
    };
    // The fixture confirms every chosen folder as /remote/work tree.
    start("/linked","linked",{});QCOMPARE(launches.size(),1);QCOMPARE(paths,QStringList{"/repo"});
    start("/elsewhere","outside","This folder is outside the project.\nStarting here will add it to this project.");
    QCOMPARE(launches.size(),2);QCOMPARE(paths,(QStringList{"/repo","/remote/work tree"}));
}

void TestSessionsWindow::automaticLaunchWorktree_data()
{
    QTest::addColumn<QString>("mode");QTest::addColumn<QString>("host");
    QTest::newRow("local-auto")<<QString("created")<<QString();
    QTest::newRow("remote-auto")<<QString("created")<<QString("mac");
    QTest::newRow("outside-auto")<<QString("outside")<<QString("mac");
    QTest::newRow("no-worktree")<<QString("none")<<QString();
    QTest::newRow("ordinary-folder")<<QString("not_repo")<<QString();
    QTest::newRow("creation-failed")<<QString("failed")<<QString("mac");
    QTest::newRow("unconfirmed-receipt")<<QString("wrong_receipt")<<QString();
}

void TestSessionsWindow::automaticLaunchWorktree()
{
    QFETCH(QString,mode);QFETCH(QString,host);
    QTemporaryDir fixture;QFile program(fixture.filePath("hgs"));QVERIFY(program.open(QIODevice::WriteOnly));
    program.write(R"PY(#!/usr/bin/env python3
import json, pathlib, sys, time
root = pathlib.Path(__file__).parent
mode = (root / 'mode').read_text()
args = sys.argv[1:]
with (root / 'calls').open('a') as log:
    log.write(json.dumps(args) + '\n')
if args and args[0].startswith('@'):
    args.pop(0)
def option(name):
    return args[args.index(name) + 1]
if args[0] == 'account':
    print(json.dumps({'profiles': [{'id': 'native-codex', 'provider': 'codex', 'label': 'Native account', 'is_default': True}]}))
elif args[0] == 'dirs':
    print(json.dumps({'path': args[-1], 'directories': []}))
elif args[:2] == ['worktrees', 'create']:
    deadline = time.monotonic() + 10
    while not (root / 'release').exists() and time.monotonic() < deadline:
        time.sleep(.02)
    if mode == 'failed':
        print('Synthetic checkout failure', file=sys.stderr)
        sys.exit(1)
    print(json.dumps({'status': 'created', 'request_id': 'foreign-request' if mode == 'wrong_receipt' else option('--request-id'),
                     'branch': option('--branch'), 'common_dir': option('--common-dir'), 'path': option('--destination')}))
elif args[0] == 'worktrees':
    print(json.dumps({'state': 'not_repo' if mode == 'not_repo' else 'ok', 'path': args[1], 'selected_root': '/repo',
                     'common_dir': '/repo/.git', 'sampled_at': int(time.time()),
                     'worktrees': [] if mode == 'not_repo' else [{'path': '/repo', 'kind': 'main', 'available': True}]}))
)PY");program.close();
    QVERIFY(FakeCli::makeRunnable(program.fileName()));
    QFile modeFile(fixture.filePath("mode"));QVERIFY(modeFile.open(QIODevice::WriteOnly));modeFile.write(mode.toUtf8());modeFile.close();
    QFile release(fixture.filePath("release"));
    const auto releaseRequest=[&]{if(!release.exists()){release.open(QIODevice::WriteOnly);release.close();}};
    const auto cleanup=qScopeGuard(releaseRequest);
    const auto creations=[&]{
        QList<QJsonArray> result;QFile calls(fixture.filePath("calls"));if(!calls.open(QIODevice::ReadOnly))return result;
        while(!calls.atEnd()){
            const auto args=QJsonDocument::fromJson(calls.readLine()).array();
            if(args.contains(QString("create")))result.append(args);
        }
        return result;
    };
    SessionOrganization projects;const auto project=projects.createGroup("Launch");
    // An outside repository joins the project, never the worktree generated from it.
    projects.addFolder(project,host.isEmpty()?"arch":host,mode=="outside"?"/unrelated":"/repo");
    NewSessionDialog dialog(FakeCli::program(program.fileName()),fleet(),host,"codex",project);dialog.setGroups(projects,project);dialog.show();
    if(mode=="outside")dialog.selectPath("/repo");
    auto *automatic=dialog.findChild<QPushButton *>("automaticLaunchWorktree");
    auto *none=dialog.findChild<QPushButton *>("launchWithoutWorktree");
    auto *start=dialog.findChild<QPushButton *>("primary");
    auto *name=dialog.findChild<QLineEdit *>("launchName");
    auto *target=dialog.findChild<QLabel *>("launchWorktreeTarget");
    QSignalSpy launched(&dialog,&NewSessionDialog::launchRequested);
    QTRY_VERIFY(start->isEnabled());
    QCOMPARE(creations().size(),0);QTRY_COMPARE(dialog.findChild<QLabel *>("muted")->property("outsideProject").toBool(),mode=="outside");
    auto *manage=dialog.findChild<QToolButton *>("manageLaunchProject");QVERIFY(manage->text().isEmpty());QVERIFY(!manage->icon().isNull());
    auto *agents=dialog.findChild<QListWidget *>("launchAccount");for(int i=0;i<agents->count();++i)QVERIFY(!agents->item(i)->icon().isNull());
    const auto generated=name->text();const auto destination="/repo-"+generated;
    name->setText("review-session");
    if(mode=="not_repo"){
        QVERIFY(!automatic->isEnabled());QVERIFY(none->isChecked());QVERIFY(target->text().isEmpty());
    }else{
        QVERIFY(automatic->isChecked());QCOMPARE(target->text(),destination);
        dialog.setFleet(fleet());QCOMPARE(creations().size(),0);
    }
    if(mode=="none")none->click();
    start->click();
    if(mode=="none"||mode=="not_repo"){
        QTRY_COMPARE(launched.size(),1);QCOMPARE(launched[0][2].toString(),QString("/repo"));QVERIFY(launched[0][7].toString().isEmpty());QCOMPARE(creations().size(),0);return;
    }
    QTRY_COMPARE(creations().size(),1);QVERIFY(!start->isEnabled());QVERIFY(!name->isEnabled());
    QVERIFY(!dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->isEnabled());
    dialog.reject();QVERIFY(dialog.isVisible());
    dialog.setFleet(fleet());QCOMPARE(dialog.findChild<QComboBox *>("launchComputer")->currentData().toString(),host);
    const auto args=creations().first();
    const auto option=[&](const QString &key){
        for(int i=0;i+1<args.size();++i)if(args.at(i).toString()==key)return args.at(i+1).toString();
        return QString();
    };
    QCOMPARE(option("--path"),QString("/repo"));QCOMPARE(option("--common-dir"),QString("/repo/.git"));
    QCOMPARE(option("--branch"),generated);QCOMPARE(option("--base"),QString("HEAD"));QCOMPARE(option("--destination"),destination);
    QVERIFY(!QUuid(option("--request-id")).isNull());if(!host.isEmpty())QCOMPARE(args.first().toString(),"@"+host);
    releaseRequest();
    if(mode=="created"||mode=="outside"){
        QTRY_COMPARE(launched.size(),1);QCOMPARE(launched[0][0].toString(),host);QCOMPARE(launched[0][2].toString(),destination);
        QCOMPARE(launched[0][3].toString(),QString("review-session"));QCOMPARE(launched[0][4].toString(),QString("native-codex"));
        QCOMPARE(launched[0][5].toString(),project);QCOMPARE(launched[0][7].toString(),mode=="outside"?QString("/repo"):QString());
    }else{
        QTRY_VERIFY(name->isEnabled());QVERIFY(!start->isEnabled());QCOMPARE(launched.size(),0);QVERIFY(dialog.isVisible());
        QVERIFY(!dialog.findChild<QLabel *>("launchError")->text().isEmpty());QCOMPARE(name->text(),QString("review-session"));
        start->click();dialog.setFleet(fleet());QCOMPARE(creations().size(),1);QCOMPARE(launched.size(),0);
        none->click();QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launched.size(),1);
        QCOMPARE(launched[0][2].toString(),QString("/repo"));
    }
    QCOMPARE(creations().size(),1);
}

void TestSessionsWindow::arbitraryFolderAndTerminalPreference_data()
{
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark")<<QString("dark");QTest::newRow("light")<<QString("light");
}
void TestSessionsWindow::arbitraryFolderAndTerminalPreference()
{
    QFETCH(QString,theme);QSettings().setValue("workspace/theme",theme);
    QTemporaryDir folderFixture; QFile folderScript(folderFixture.filePath("hgs"));
    QFile original(script()); QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(folderScript.open(QIODevice::WriteOnly));
    folderScript.write(original.readAll().replace("\"path\":\"/remote/work tree\"", "\"path\":\"/remote/work tree/\"")); folderScript.close();
    QVERIFY(FakeCli::makeRunnable(folderScript.fileName()));
    SessionsWindow parent(program());parent.setFleet(fleet());parent.show();
    SessionOrganization projects;const auto id=projects.createGroup("infra"),other=projects.createGroup("Other");
    projects.addFolder(other,"mac","/remote/other");
    NewSessionDialog dialog(FakeCli::program(folderScript.fileName()),fleet(),"mac","codex",{},&parent);dialog.setGroups(projects,id);dialog.show();
    auto *folders=dialog.findChild<QComboBox *>("launchProjectFolder");auto *start=dialog.findChild<QPushButton *>("primary");
    auto *terminal=dialog.findChild<QCheckBox *>("launchOpenTerminal");auto *browse=dialog.findChild<QPushButton *>("browseLaunchFolder");
    QCOMPARE(folders->count(),0);QVERIFY(!start->isEnabled());QVERIFY(!terminal->isChecked());
    QTimer::singleShot(0,&dialog,[&]{
        auto *browser=qobject_cast<DirectoryDialog *>(QApplication::activeModalWidget());QVERIFY(browser);browser->reject();
    });browse->click();QCOMPARE(folders->count(),0);
    QTimer::singleShot(0,&dialog,[&]{
        auto *browser=qobject_cast<DirectoryDialog *>(QApplication::activeModalWidget());QVERIFY(browser);
        QTimer::singleShot(3000,browser,&QDialog::reject);
        auto *choose=browser->findChild<QPushButton *>("chooseFolder");QTRY_VERIFY(choose->isEnabled());choose->click();
    });browse->click();QCOMPARE(folders->count(),1);QTRY_VERIFY(start->isEnabled());
    QCOMPARE(folders->currentText(),QString("work tree"));
    auto *project=dialog.findChild<QComboBox *>("launchProject");
    project->setCurrentIndex(project->findData(other));
    QCOMPARE(folders->currentData(Qt::UserRole+2).toString(),QString("/remote/work tree"));
    QVERIFY(dialog.findChild<QLabel *>("muted")->property("outsideProject").toBool());
    project->setCurrentIndex(project->findData(id));
    QCOMPARE(dialog.findChild<QLabel *>("launchFolderPath")->text(),QString("/remote/work tree"));
    QVERIFY(projects.group(id)->folders.isEmpty());
    dialog.setFleet(fleet());QCOMPARE(folders->currentData(Qt::UserRole+2).toString(),QString("/remote/work tree"));
    auto *machine=dialog.findChild<QComboBox *>("launchComputer");machine->setCurrentIndex(machine->findData(QString()));
    QCOMPARE(folders->count(),0);QVERIFY(!start->isEnabled());machine->setCurrentIndex(machine->findData("mac"));QCOMPARE(folders->count(),1);
    const auto preview=qEnvironmentVariable("HGS_NAVIGATION_PREVIEW");
    if(!preview.isEmpty()){QDir().mkpath(preview);QTest::qWait(40);QVERIFY(dialog.grab().save(preview+"/new-session-folder-"+theme+".png"));}
    QSignalSpy launches(&dialog,&NewSessionDialog::launchRequested);terminal->setChecked(true);QTRY_VERIFY(start->isEnabled());start->click();QTRY_COMPARE(launches.size(),1);
    QCOMPARE(launches[0][0].toString(),QString("mac"));QVERIFY(launches[0][6].toBool());QVERIFY(!launches[0][7].toString().isEmpty());QCOMPARE(launches[0][7].toString(),launches[0][2].toString());
    NewSessionDialog second(program(),fleet(),"mac");QVERIFY(second.findChild<QCheckBox *>("launchOpenTerminal")->isChecked());
    second.setGroups(projects,id);QCOMPARE(second.findChild<QComboBox *>("launchProjectFolder")->count(),0);
    projects.addFolder(id,"mac","/remote/work tree");second.setGroups(projects,id);
    second.findChild<QCheckBox *>("launchOpenTerminal")->setChecked(false);
    QSignalSpy secondLaunch(&second,&NewSessionDialog::launchRequested);QTRY_VERIFY(second.findChild<QPushButton *>("primary")->isEnabled());second.findChild<QPushButton *>("primary")->click();QTRY_COMPARE(secondLaunch.size(),1);
    QVERIFY(!secondLaunch[0][6].toBool());QVERIFY(secondLaunch[0][7].toString().isEmpty());
    NewSessionDialog third(program(),fleet());QVERIFY(!third.findChild<QCheckBox *>("launchOpenTerminal")->isChecked());
    // Selecting a project folder explicitly replaces the earlier Browse choice.
    project->setCurrentIndex(project->findData(other));
    folders->setCurrentIndex(folders->findData(QString("/remote/other"),Qt::UserRole+2));
    project->setCurrentIndex(project->findData(id));QCOMPARE(folders->count(),0);
}

void TestSessionsWindow::terminationProgress_data()
{
    QTest::addColumn<QString>("mode");
    for(const auto &mode:{"success","failure","offline","replacement","archive","search"})QTest::newRow(mode)<<QString(mode);
}
void TestSessionsWindow::terminationProgress()
{
    QFETCH(QString,mode);
    QTemporaryDir directory;QFile fake(directory.filePath("hgs"));QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write(R"(#!/usr/bin/env python3
import json,sys,time,pathlib
args=sys.argv[1:]
if args and args[0].startswith('@'):args=args[1:]
if args and args[0] in ('kill','terminate'):
 time.sleep(0.3)
 if pathlib.Path(__file__).with_name('fail').exists():
  print('Could not stop this session',file=sys.stderr);sys.exit(1)
print(json.dumps({'tracked':True,'events':[],'cursor':0}))
)");fake.close();QVERIFY(FakeCli::makeRunnable(fake.fileName()));
    if(mode=="failure"){QFile failure(directory.filePath("fail"));QVERIFY(failure.open(QIODevice::WriteOnly));}
    SessionOrganization organization;const auto group=organization.createGroup("Work");organization.moveSession("arch\ncodex/hgs/dashboard",group);
    QSettings().setValue("workspace/organization",QJsonDocument(organization.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(FakeCli::program(fake.fileName()));auto state=fleet();window.setFleet(state);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *list=window.findChild<SessionList *>("sessionList");auto *client=window.findChild<HgsClient *>();
    auto *composer=window.findChild<MessageComposer *>("messageComposer");
    setComposerText(composer,"Keep my draft after termination");
    if(mode=="search")window.findChild<QLineEdit *>("search")->setText("dashboard");
    QSignalSpy writes(client,&HgsClient::writeDone);QSignalSpy refresh(&window,&SessionsWindow::refreshRequested);
    const auto row=[&]() -> QListWidgetItem * {for(int i=0;i<list->count();++i)if(list->item(i)->data(SessionRoles::Key).toString()=="\ncodex/hgs/dashboard")return list->item(i);return nullptr;};
    QTimer::singleShot(0,&window,[&]{auto *question=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());QVERIFY(question);question->button(QMessageBox::Yes)->click();});
    window.findChild<QAction *>("forgetSessionAction")->trigger();
    QVERIFY(row());QCOMPARE(row()->data(SessionRoles::Status).toString(),QString("Terminating…"));
    QCOMPARE(window.findChild<QLabel *>("badge")->text(),QString("Terminating…"));
    QVERIFY(!window.findChild<QAction *>("pauseAction")->isEnabled());
    window.setFleet(state);QCOMPARE(row()->data(SessionRoles::Status).toString(),QString("Terminating…"));
    QTRY_COMPARE(writes.size(),1);QVERIFY(!refresh.isEmpty());
    if(mode=="failure"){
        QVERIFY(row()->data(SessionRoles::Status).toString()!="Terminating…");
        QCOMPARE(composer->sessionKey(),QString("\ncodex/hgs/dashboard"));
        QCOMPARE(composer->editor()->toPlainText(),QString("Keep my draft after termination"));
        bool error=false;for(auto *label:window.findChildren<QLabel *>())error|=label->text().contains("Could not stop this session");QVERIFY(error);return;
    }
    window.setFleet(state);QCOMPARE(row()->data(SessionRoles::Status).toString(),QString("Terminating…"));
    // A second activation while waiting for the next snapshot must not issue another kill.
    window.findChild<QAction *>("forgetSessionAction")->trigger();QCOMPARE(writes.size(),1);
    auto local=state.local();
    if(mode=="offline"){
        local.ok=false;state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
        QCOMPARE(row()->data(SessionRoles::Status).toString(),QString("Terminating…"));local.ok=true;
    }
    if(mode=="replacement"){
        local.sessions[0].runId="replacement-run";local.sessions[0].created+=1;
        state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
        QVERIFY(row());QVERIFY(row()->data(SessionRoles::Status).toString()!="Terminating…");return;
    }
    if(mode=="archive"){
        auto archived=local.sessions[0];archived.state="archived";archived.archiveId="terminated-archive";local.sessions.append(archived);
    }
    local.sessions.removeFirst();state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);QVERIFY(!row());
    QVERIFY(!list->currentItem());QVERIFY(list->selectedItems().isEmpty());
    QCOMPARE(composer->sessionKey(),QString());
    QCOMPARE(window.findChild<QStackedWidget *>("detail")->currentIndex(),0);
    ComposerDraftStore::flushWrites();
    QCOMPARE(ComposerDraftStore().load("\ncodex/hgs/dashboard").text,QString("Keep my draft after termination"));
    // Polling and a newly arriving session cannot choose another conversation.
    window.setFleet(state);QVERIFY(!list->currentItem());QCOMPARE(composer->sessionKey(),QString());
    auto incoming=local.sessions[0];incoming.name="codex/new/incoming";local.sessions.append(incoming);
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    window.findChild<QLineEdit *>("search")->clear();
    QVERIFY(!list->currentItem());QCOMPARE(composer->sessionKey(),QString());
    if(mode=="archive"){
        SessionOrganization saved(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
        QCOMPARE(saved.groupFor("arch\narchive\nterminated-archive"),group);
    }
    QListWidgetItem *next=nullptr;
    for(int i=0;i<list->count();++i)if(!list->item(i)->isHidden()&&(list->item(i)->flags()&Qt::ItemIsSelectable)){next=list->item(i);break;}
    QVERIFY(next);const auto nextKey=next->data(SessionRoles::Key).toString();list->scrollToItem(next);
    QTest::mouseClick(list->viewport(),Qt::LeftButton,{},list->visualItemRect(next).center());
    QCOMPARE(composer->sessionKey(),nextKey);QCOMPARE(window.findChild<QStackedWidget *>("detail")->currentIndex(),1);
}

void TestSessionsWindow::listTerminationKeepsUnrelatedConversations_data()
{
    QTest::addColumn<QString>("mode");
    for(const auto *mode:{"context","bulk-current","bulk-other"})QTest::newRow(mode)<<QString::fromLatin1(mode);
}
void TestSessionsWindow::listTerminationKeepsUnrelatedConversations()
{
    QFETCH(QString,mode);
    auto state=fleet();auto local=state.local();
    SessionsWindow window(program());window.setFleet(state);window.resize(1100,850);window.show();window.showSession({},local.sessions[0].name);
    auto *list=window.findChild<SessionList *>("sessionList");auto *composer=window.findChild<MessageComposer *>("messageComposer");
    const auto key=composer->sessionKey();setComposerText(composer,"Keep the current draft");
    const auto toggle=[&](int index){
        QListWidgetItem *item=nullptr;
        for(int i=0;i<list->count();++i)if(list->item(i)->data(SessionRoles::Key).toString()=='\n'+local.sessions[index].name){item=list->item(i);break;}
        QVERIFY(item);list->scrollToItem(item);QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::ControlModifier,list->visualItemRect(item).center());
    };
    QSet<QString> removed;
    QAction *action=nullptr;
    if(mode=="context"){
        QTest::keyClick(list,Qt::Key_Menu);auto *menu=window.findChild<QMenu *>("sessionContextMenu");
        action=menu->findChild<QAction *>("contextTerminateSession");removed={local.sessions[0].name};menu->hide();
    }else{
        if(mode=="bulk-other"){toggle(0);toggle(1);toggle(3);removed={local.sessions[1].name,local.sessions[3].name};}
        else{toggle(1);removed={local.sessions[0].name,local.sessions[1].name};}
        QSet<QString> chosen;for(const auto *item:list->selectedItems())chosen.insert(item->data(SessionRoles::Key).toString().section('\n',1,1));
        QCOMPARE(chosen,removed);
        action=window.findChild<QAction *>("selectionstop");
    }
    QVERIFY(action);QVERIFY(action->isEnabled());
    QTimer::singleShot(0,&window,[&]{auto *dialog=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());QVERIFY(dialog);
        if(auto *yes=dialog->button(QMessageBox::Yes))yes->click();
        else for(auto *button:dialog->buttons())if(dialog->buttonRole(button)==QMessageBox::DestructiveRole){button->click();break;}
    });
    QSignalSpy writes(window.findChild<HgsClient *>(),&HgsClient::writeDone);action->trigger();QTRY_COMPARE(writes.size(),removed.size());
    for(const auto &write:writes)QVERIFY(write[1].toBool());
    local.sessions.removeIf([&](const SessionInfo &session){return removed.contains(session.name);});
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);window.setFleet(state);
    if(mode=="bulk-other"){
        QCOMPARE(composer->sessionKey(),key);QCOMPARE(composer->editor()->toPlainText(),QString("Keep the current draft"));
        QVERIFY(list->currentItem());QCOMPARE(list->currentItem()->data(SessionRoles::Key).toString(),key);
    }else{
        QVERIFY(!list->currentItem());QVERIFY(list->selectedItems().isEmpty());QCOMPARE(composer->sessionKey(),QString());
        QCOMPARE(window.findChild<QStackedWidget *>("detail")->currentIndex(),0);
    }
}

void TestSessionsWindow::remoteFolderLaunch()
{
    NewSessionDialog dialog(program(), fleet(), "mac"); dialog.show();
    auto *projects = dialog.findChild<QComboBox *>("launchProject"); QTRY_COMPARE(projects->count(), 1);
    SessionOrganization organization; organization.addFolder("ungrouped","mac","/remote/work tree");
    dialog.setGroups(organization,"ungrouped");
    const auto name = dialog.findChild<QLineEdit *>("launchName")->text(); QVERIFY(name.startsWith("work-"));
    QSignalSpy launched(&dialog, &NewSessionDialog::launchRequested);
    auto *start = dialog.findChild<QPushButton *>("primary"); QTRY_VERIFY(start->isEnabled()); start->click();
    QTRY_COMPARE(launched.size(), 1);
    QCOMPARE(launched[0][0].toString(), QStringLiteral("mac"));
    QCOMPARE(launched[0][1].toString(), QStringLiteral("codex"));
    QCOMPARE(launched[0][2].toString(), QStringLiteral("/remote/work tree"));
    QCOMPARE(launched[0][3].toString(), name);
    QCOMPARE(launched[0][5].toString(), QString("ungrouped"));
    NewSessionDialog second(program(), fleet(), {}, "martty-dsh", "infra");
    QVERIFY(second.findChild<QLineEdit *>("launchName")->text() != name);
    auto *accounts=second.findChild<QListWidget *>("launchAccount");
    QTRY_VERIFY(accounts->currentItem()&&accounts->currentItem()->data(Qt::UserRole+1).toString()=="codex");
    for(int row=0;row<accounts->count();++row)QVERIFY(accounts->item(row)->data(Qt::UserRole+1).toString()!="martty-dsh");
}

void TestSessionsWindow::launchNameFollowsRenameRules()
{
    // Creating a session accepts exactly the tags that Rename accepts.
    NewSessionDialog dialog(program(), fleet(), "mac"); dialog.show();
    QTRY_COMPARE(dialog.findChild<QComboBox *>("launchProject")->count(), 1);
    SessionOrganization organization; organization.addFolder("ungrouped","mac","/remote/work tree");
    dialog.setGroups(organization,"ungrouped");
    auto *name = dialog.findChild<QLineEdit *>("launchName");
    auto *problem = dialog.findChild<QLabel *>("launchNameError"); QVERIFY(problem);
    auto *start = dialog.findChild<QPushButton *>("primary"); QTRY_VERIFY(start->isEnabled());
    for (const QString &bad : {QString(" plan"), QString("plan "), QString("a/b"), QString("a.b"), QString("a:b"), QString()}) {
        name->setText(bad);
        QVERIFY2(!start->isEnabled(), qPrintable(bad));
        QVERIFY2(problem->isVisible() && !problem->text().isEmpty(), qPrintable(bad));
    }
    name->setText("my plan");
    QTRY_VERIFY(start->isEnabled()); QVERIFY(!problem->isVisible());
    QSignalSpy launched(&dialog, &NewSessionDialog::launchRequested);
    start->click();
    QTRY_COMPARE(launched.size(), 1);
    QCOMPARE(launched[0][3].toString(), QStringLiteral("my plan"));
}

void TestSessionsWindow::openCodeModelLaunchUsesNativeAccountAndActivity()
{
    QTemporaryDir fixture;const auto fake=fixture.filePath("hgs");
    const QString longId = "ollama/" + QString(249, 'x');
    const QString unicodeId = "ollama/" + QString(124, QChar(0x00e9)) + "x";
    const QString id121 = "ollama/" + QString(114, 'x');
    QCOMPARE(longId.toUtf8().size(), 256); QCOMPARE(unicodeId.toUtf8().size(), 256);
    const QJsonArray modelsOffered{
        QJsonObject{{"value","ollama/qwen3:8b"},{"label","Qwen 3"}},
        QJsonObject{{"value","ollama/qwen3:14b"},{"label","Qwen 3 large"}},
        QJsonObject{{"value",id121}},QJsonObject{{"value",longId}},QJsonObject{{"value",unicodeId}},
        QJsonObject{{"value",longId+"x"}},QJsonObject{{"value","ollama/"+QString(125,QChar(0x00e9))}}};
    const QJsonObject accounts{{"profiles",QJsonArray{
        QJsonObject{{"id","native-opencode"},{"provider","opencode"},{"label","Native configuration"},{"installed",true},{"is_default",true},{"model_options",modelsOffered}},
        QJsonObject{{"id","native-codex"},{"provider","codex"},{"label","Native account"},{"installed",true}}}}};
    QVERIFY(FakeCli::install(fake,"#!/usr/bin/env python3\nimport json,sys\na=sys.argv[1:]\nif a and a[0].startswith('@'):a=a[1:]\n"
        "if a[0]=='account':print("+FakeCli::pythonString(QString::fromUtf8(QJsonDocument(accounts).toJson(QJsonDocument::Compact)))+")\n"
        "elif a[0]=='dirs':print('{\"path\":\"/fixture/project\",\"directories\":[]}')\n"
        "elif a[0]=='worktrees':print('{\"state\":\"not_repo\"}')\n"));
    QSettings().setValue("workspace/launchOpenTerminal",true);
    // This computer reports Linux: without a platform a Windows build takes it for
    // Windows, which offers no OpenCode (spec D16).
    auto state=fleet();auto local=state.local();local.metrics=QJsonObject{{"platform","linux"}};state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    NewSessionDialog dialog(FakeCli::program(fake),state,{},"opencode");dialog.selectPath("/fixture/project",false);dialog.show();
    auto *start=dialog.findChild<QPushButton *>("primary"),*none=dialog.findChild<QPushButton *>("launchWithoutWorktree");
    auto *account=dialog.findChild<QListWidget *>("launchAccount");auto *models=dialog.findChild<QComboBox *>("launchModel");
    QVERIFY(account);QVERIFY(models);QVERIFY(!dialog.findChild<QComboBox *>("launchAgent"));
    auto *terminal=dialog.findChild<QCheckBox *>("launchOpenTerminal");
    QTRY_VERIFY(start->isEnabled());QTRY_COMPARE(models->count(),6);
    QCOMPARE(account->currentItem()->data(Qt::UserRole).toString(),QString("native-opencode"));QVERIFY(models->isVisible());QVERIFY(!terminal->isEnabled());
    QVERIFY(terminal->toolTip().contains("Activity"));QVERIFY(none->isChecked());
    QCOMPARE(models->currentData().toString(),QString());models->setCurrentIndex(models->findData("ollama/qwen3:8b"));
    QVERIFY(models->findData(id121)>=0);QVERIFY(models->findData(longId)>=0);QVERIFY(models->findData(unicodeId)>=0);
    QCOMPARE(models->findData(longId+"x"),-1);QCOMPARE(models->findData("ollama/"+QString(125,QChar(0x00e9))),-1);
    const auto selectAccount=[&](const QString &id){for(int i=0;i<account->count();++i)if(account->item(i)->data(Qt::UserRole).toString()==id){account->setCurrentRow(i);return true;}return false;};
    QVERIFY(selectAccount("native-codex"));QVERIFY(!models->isVisible());QCOMPARE(models->currentData().toString(),QString());
    QVERIFY(selectAccount("native-opencode"));QTRY_COMPARE(models->count(),6);models->setCurrentIndex(models->findData("ollama/qwen3:8b"));
    QVERIFY(models->geometry().right()<dialog.width());QVERIFY(start->geometry().bottom()<dialog.height());
    QSignalSpy launch(&dialog,&NewSessionDialog::launchRequested);start->click();QTRY_COMPARE(launch.size(),1);
    QCOMPARE(launch[0][1].toString(),QString("opencode"));QCOMPARE(launch[0][2].toString(),QString("/fixture/project"));
    QCOMPARE(launch[0][4].toString(),QString("native-opencode"));QVERIFY(!launch[0][6].toBool());QCOMPARE(launch[0][8].toString(),QString("ollama/qwen3:8b"));
    QSettings().remove("workspace/launchOpenTerminal");QSettings().remove("workspace/launchAgent");QSettings().remove("workspace/launchAccounts");
}

void TestSessionsWindow::machineBatchCountsExcludeNativeOpenCodeSessions()
{
    BoxState box;box.host="fixture";box.ok=true;
    SessionInfo terminal;terminal.name="codex/project/terminal";terminal.cmd="codex";terminal.resumable=true;terminal.activity="idle";
    auto api=terminal;api.name="opencode/project/api";api.cmd="opencode";api.activity="busy";
    auto saved=api;saved.name="opencode/project/saved";saved.state="paused";saved.activity="idle";
    box.sessions={terminal,api,saved};FleetState state;state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.setFleet(state);
    auto *menu=window.findChild<QPushButton *>("batchActions")->menu();QMetaObject::invokeMethod(menu,"aboutToShow");
    auto *machine=menu->actions().first()->menu();QVERIFY(machine);
    const auto action=[&](const QString &text){for(auto *entry:machine->actions())if(entry->text()==text)return entry;return static_cast<QAction *>(nullptr);};
    auto *pause=action("Pause all (1)"),*resume=action("Resume all in background (0)");QVERIFY(pause);QVERIFY(resume);
    QVERIFY(pause->isEnabled());QVERIFY(!resume->isEnabled());QVERIFY(pause->toolTip().contains("OpenCode sessions are excluded"));
    box.sessions={api,saved};state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);QMetaObject::invokeMethod(menu,"aboutToShow");
    machine=menu->actions().first()->menu();pause=action("Pause all (0)");resume=action("Resume all in background (0)");
    QVERIFY(pause);QVERIFY(resume);QVERIFY(!pause->isEnabled());QVERIFY(!resume->isEnabled());
}

void TestSessionsWindow::openCodeCapabilitiesPreserveDraftWithoutTerminal()
{
    const QString name="opencode/project/local",run="open-run",conversation="open-session";
    const QJsonObject details{{"name",name},{"cmd","opencode"},{"tracked",true},{"state","running"},{"run_id",run},{"conversation_id",conversation},
        {"phase","idle"},{"activity","idle"},{"process_state","running"},{"runtime_state","live"},{"resumable",true},
        {"terminal_supported",false},{"terminal_reason","OpenCode runs through its native API"},{"attachments_supported",false},
        {"clear_context_supported",false},{"compact_context_supported",false},{"fork_supported",false},
        {"allowed_actions",QJsonArray{"pause","rename"}},{"events",QJsonArray{}},{"cursor",0}};
    const auto fake=programInspecting("opencode-capabilities",details);QVERIFY(!fake.isEmpty());
    auto state=fleet();auto box=state.local();SessionInfo session;session.name=name;session.cmd="opencode";session.runId=run;session.conversationId=conversation;
    session.tracked=true;session.resumable=true;session.activity="idle";session.phase="idle";session.processState="running";session.runtimeState="live";
    session.terminalSupported=false;session.actionsReported=true;session.allowedActions={"pause","rename"};box.sessions={session};state.setLocal(box,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(fake);window.setFleet(state);window.show();window.showSession({},name);
    auto *composer=window.findChild<MessageComposer *>("messageComposer");auto *client=window.findChild<HgsClient *>();
    settleSelectedInspection(composer,client,{},name,details);setComposerText(composer,"Keep my local model draft");
    auto *tabs=window.findChild<QTabWidget *>("sessionDetailTabs");QVERIFY(tabs);QVERIFY(!tabs->isTabVisible(1));QVERIFY(!tabs->isTabVisible(2));
    QTRY_VERIFY2(composer->findChild<QPushButton *>("sendMessage")->isEnabled(),qPrintable(composer->findChild<QLabel *>("messageStatus")->text()+" key="+composer->sessionKey()+" draft="+composer->editor()->toPlainText()));QVERIFY(!composer->canAttachFiles());
    QVERIFY(!composer->findChild<QPushButton *>("attachMessageFile")->isEnabled());
    QVERIFY(!window.findChild<QPushButton *>("clearSessionAction")->isEnabled());QVERIFY(!window.findChild<QAction *>("forgetSessionAction")->isEnabled());
    QCOMPARE(composer->editor()->toPlainText(),QString("Keep my local model draft"));QVERIFY(!composer->editor()->isReadOnly());
}

void TestSessionsWindow::nativeLaunchStaysInWorkspace_data()
{
    QTest::addColumn<QString>("agent");
    QTest::newRow("native")<<QString("dsh");QTest::newRow("codex-detached")<<QString("codex");
}
void TestSessionsWindow::nativeLaunchStaysInWorkspace()
{
    QFETCH(QString,agent);
    SessionOrganization projects; projects.addFolder("ungrouped","arch","/workspace/infra");
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    // This computer reports Linux: without a platform a Windows build takes it for
    // Windows, which offers no DeepSeek (spec D16).
    auto state=fleet();auto local=state.local();local.metrics=QJsonObject{{"platform","linux"}};state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.setFleet(state);window.show();
    constexpr int noticeMs=250;window.setNoticeDurationForTests(noticeMs);
    QSignalSpy terminal(&window,&SessionsWindow::newSessionRequested);
    auto *client=window.findChild<HgsClient *>();QSignalSpy launched(client,&HgsClient::nativeSessionLaunched);
    QTimer::singleShot(0,&window,[&] {
        auto *dialog=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
        QTimer::singleShot(20000,dialog,&QDialog::reject);
        QTRY_VERIFY_WITH_TIMEOUT(dialog->findChild<QPushButton *>("primary")->isEnabled(),15000);
        dialog->findChild<QLineEdit *>("launchName")->setText("native-created");
        dialog->findChild<QComboBox *>("launchProject")->setCurrentIndex(0);
        dialog->findChild<QPushButton *>("primary")->click();
    });
    window.showNewSession(agent,"infra",{});
    QTRY_COMPARE(launched.size(),1);QCOMPARE(terminal.size(),0);QVERIFY(launched[0][1].toBool());
    const QString token=launched[0][0].toString();QVERIFY(!token.isEmpty());
    auto box=state.local();SessionInfo created;created.name=agent+"/infra/native-created";created.cmd=agent;created.project="infra";created.tag="native-created";
    created.launchId=token;created.runtimeState="live";created.processState="running";created.tracked=true;created.resumable=true;
    box.sessions.append(created);state.setLocal(box,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    QCOMPARE(window.findChild<SessionList *>("sessionList")->currentItem()->data(SessionRoles::Identity).toString(),"arch\n"+created.name);
    QTest::qWait(20);
    auto *list = window.findChild<SessionList *>("sessionList");
    const auto bounds = [&] { return QRect(list->mapTo(&window, QPoint()), list->size()); };
    const auto originalBounds = bounds();
    auto *statusBar = window.findChild<QFrame *>("workspaceStatus"); QVERIFY(statusBar); QCOMPARE(statusBar->height(), 28);
    window.showNotificationNotice(QString(2000, 'x') + "\nRefreshing…"); QCoreApplication::processEvents();
    QCOMPARE(bounds(), originalBounds);
    // A native failure must stay visible after the previous success notice expires.
    client->nativeSessionLaunched("failed-launch",false,"Cannot find official dsh");
    QLabel *notice=nullptr;
    for(auto *label:window.findChildren<QLabel *>()) if(label->text().contains("Cannot find official dsh")) notice=label;
    QVERIFY(notice);QVERIFY(notice->isVisible());QTest::qWait(3*noticeMs);QVERIFY(notice->isVisible());
    QCOMPARE(bounds(), originalBounds);
    window.showNotificationNotice("Session updated.");
    QTRY_VERIFY(notice->text().isEmpty());
    QVERIFY(statusBar->isVisible()); QCOMPARE(bounds(), originalBounds);
}

namespace {
// Stands in for the optional embedded native UI plugin (NativeBrowserPlugin.h).
struct FakeNativeBrowsers final : NativeBrowserFactory {
    struct Browser final : NativeBrowser {
        QWidget *widget() override { return &view; }
        void load(const QUrl &) override {}
        QWidget view;
    };
    NativeBrowser *create(const QString &, const QColor &, QWidget *) override { ++created; return new Browser; }
    int created = 0;
};
}

void TestSessionsWindow::nativeSignInPreservesDraftAndUnblocks()
{
    FakeNativeBrowsers plugin; NativeBrowserPlugin::setFactoryForTesting(&plugin);
    const auto restore = qScopeGuard([] { NativeBrowserPlugin::setFactoryForTesting(nullptr); });
    auto state = fleet(); auto box = state.local();
    SessionInfo session; session.name = "dsh/infra/new"; session.cmd = "dsh"; session.project = "infra"; session.tag = "new";
    session.tracked = true; session.resumable = true; session.state = "running"; session.runtimeState = "live"; session.processState = "running";
    box.sessions = {session}; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QTemporaryDir fixtureDir;const auto program=fixtureDir.filePath("hgs");QFile fixture(program);QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(R"PY(#!/usr/bin/env python3
import pathlib,sys
root=pathlib.Path(__file__).parent
if 'inspect' in sys.argv: print((root/'details.json').read_text())
else: print('{}')
)PY");fixture.close();QVERIFY(FakeCli::makeRunnable(fixture.fileName()));
    QJsonObject details{{"backend", "dsh"}, {"tracked", true}, {"conversation_id", "conversation-one"}, {"run_id", "native-run"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"phase", "input"}, {"activity", "attention"},
        {"activity_summary", "Sign in required"}, {"auth_required", true}, {"replace_events", true}, {"events", QJsonArray{}}};
    const auto save=[&]{return FakeCli::replace(fixtureDir.filePath("details.json"),QJsonDocument(details).toJson());};QVERIFY(save());
    SessionsWindow window(FakeCli::program(program)); window.setFleet(state); window.show(); window.showSession({}, session.name);
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    QTRY_VERIFY(!inspected.isEmpty());
    client->inspectionReady({}, session.name, details);
    auto *banner = window.findChild<QWidget *>("nativeSignInBanner"); QVERIFY(banner); QVERIFY(banner->isVisible());
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); auto *editor = composer->findChild<QPlainTextEdit *>(); QVERIFY(editor);
    setComposerText(composer, "Keep this first message");
    auto *send = composer->findChild<QPushButton *>("sendMessage"); QVERIFY(send); QVERIFY(!send->isEnabled());
    auto *activity = window.findChild<ActivityView *>("mainActivity")->browser();
    QTRY_VERIFY(activity->toPlainText().contains("After signing in"));
    // The native tab requests a private URL; this fixture intentionally has
    // no browser endpoint. It must not open an external browser automatically.
    QSignalSpy external(client, &HgsClient::writeDone), connection(client, &HgsClient::nativeUiFailed);
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
    QVERIFY(!tabs->isTabVisible(1)); QVERIFY(tabs->isTabVisible(2));
    window.findChild<QPushButton *>("nativeSignInButton")->click();
    QCOMPARE(tabs->currentWidget()->objectName(), QString("nativeUiView"));
    QTRY_COMPARE(connection.size(), 1); QCOMPARE(external.size(), 0);
    tabs->setCurrentIndex(0);
    details["auth_required"] = false; details["phase"] = "idle"; details["activity"] = "idle"; details["activity_summary"] = "Ready";QVERIFY(save());
    client->inspectionReady({}, session.name, details);
    QVERIFY(!banner->isVisible()); QVERIFY(send->isEnabled()); QCOMPARE(editor->toPlainText(), QString("Keep this first message"));
    QTRY_VERIFY2(activity->toPlainText().contains("Ready for your first message"),qPrintable(activity->toPlainText()));
}

void TestSessionsWindow::nativeUiOpensBrowserWithoutPlugin()
{
    // This suite's directory has no plugin: no Native UI tab, and the routes to
    // it hand the native UI to the system browser.
    QVERIFY(!NativeBrowserPlugin::factory());
    auto state = fleet(); auto box = state.local();
    SessionInfo session; session.name = "dsh/infra/new"; session.cmd = "dsh"; session.project = "infra"; session.tag = "new";
    session.tracked = true; session.resumable = true; session.state = "running"; session.runtimeState = "live"; session.processState = "running";
    box.sessions = {session}; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    QTemporaryDir fixtureDir; const auto script = fixtureDir.filePath("hgs");
    QVERIFY(FakeCli::install(script, R"PY(#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(__file__).parent
with open(__file__+'.calls','a') as calls: calls.write(json.dumps(sys.argv[1:])+'\n')
if 'inspect' in sys.argv: print((root/'details.json').read_text())
else: print('{}')
)PY"));
    const QJsonObject details{{"backend", "dsh"}, {"tracked", true}, {"conversation_id", "conversation-one"}, {"run_id", "native-run"},
        {"runtime_state", "live"}, {"process_state", "running"}, {"phase", "input"}, {"activity", "attention"},
        {"activity_summary", "Sign in required"}, {"auth_required", true}, {"replace_events", true}, {"events", QJsonArray{}}};
    QVERIFY(FakeCli::replace(fixtureDir.filePath("details.json"), QJsonDocument(details).toJson()));
    SessionsWindow window(FakeCli::program(script)); window.setFleet(state); window.show(); window.showSession({}, session.name);
    auto *client = window.findChild<HgsClient *>(); QSignalSpy inspected(client, &HgsClient::inspectionReady);
    QTRY_VERIFY(!inspected.isEmpty());
    client->inspectionReady({}, session.name, details);
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs");
    QVERIFY(!tabs->isTabVisible(1)); QVERIFY(!tabs->isTabVisible(2));
    QSignalSpy written(client, &HgsClient::writeDone);
    window.findChild<QPushButton *>("nativeSignInButton")->click();
    QTRY_COMPARE(written.size(), 1); QCOMPARE(tabs->currentIndex(), 0);
    QFile calls(script + ".calls"); QVERIFY(calls.open(QIODevice::ReadOnly)); bool browser = false;
    for (const auto &line : calls.readAll().split('\n')) {
        const auto call = QJsonDocument::fromJson(line).array();
        if (call.contains(QJsonValue("native-ui"))) { browser = true; QVERIFY(!call.contains(QJsonValue("--json"))); }
    }
    QVERIFY(browser);
}

void TestSessionsWindow::newSessionGroupFollowsLaunchIdentity_data()
{
    QTest::addColumn<QString>("host"); QTest::addColumn<QString>("mode");
    QTest::newRow("local-success") << QString() << QString("success");
    QTest::newRow("remote-success") << QString("mac") << QString("success");
    QTest::newRow("restart") << QString("mac") << QString("restart");
    QTest::newRow("deleted-group") << QString() << QString("deleted");
    QTest::newRow("failed-open") << QString("mac") << QString("failed");
    QTest::newRow("empty-project") << QString() << QString("empty");
    QTest::newRow("dismissed") << QString() << QString("dismissed");
    QTest::newRow("legacy-intent") << QString() << QString("legacy");
}

void TestSessionsWindow::newSessionGroupFollowsLaunchIdentity()
{
    QFETCH(QString, host); QFETCH(QString, mode);
    SessionOrganization organization;
    const QString group = organization.createGroup("Design");
    organization.addFolder(group,host.isEmpty()?QString("arch"):host,"/remote/infra");
    if (mode != "empty") organization.moveSession(organization.observe("arch", "codex/hgs/dashboard", ""), group);
    QSettings().setValue("workspace/organization", QJsonDocument(organization.toJson()).toJson(QJsonDocument::Compact));
    auto state = fleet();
    auto window = std::make_unique<SessionsWindow>(program()); window->setFleet(state); window->show(); window->showSession({}, "codex/hgs/dashboard");
    QSignalSpy launches(window.get(), &SessionsWindow::newSessionRequested);
    const QString destination = group;
    QTimer::singleShot(0, window.get(), [&] {
        auto *dialog = qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        QTimer::singleShot(20000, dialog, &QDialog::reject);
        auto *groups = dialog->findChild<QComboBox *>("launchProject"); QVERIFY(groups);
        QCOMPARE(groups->currentData().toString(), group);
        QVERIFY(groups->findData("pinned") < 0); QVERIFY(groups->findData("ungrouped") >= 0);
        groups->setCurrentIndex(groups->findData(destination));
        QCOMPARE(dialog->findChild<QComboBox *>("launchProjectFolder")->count(), 1);
        dialog->findChild<QCheckBox *>("launchOpenTerminal")->setChecked(true);
        dialog->findChild<QLineEdit *>("launchName")->setText("grouped");
        auto *start = dialog->findChild<QPushButton *>("primary"); QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 15000); start->click();
    });
    window->showNewSession("codex", group, host); QCOMPARE(launches.size(), 1);
    const QString token = launches[0][5].toString(); QVERIFY(!QUuid(token).isNull()); QCOMPARE(launches[0][0].toString(), host);
    auto placeholder = [&]() -> QListWidgetItem * {
        auto *list = window->findChild<SessionList *>("sessionList");
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::LaunchId).toString() == token) return list->item(i);
        return nullptr;
    };
    QVERIFY(placeholder()); QCOMPARE(placeholder()->data(SessionRoles::Title).toString(), QString("grouped"));
    QCOMPARE(placeholder()->data(SessionRoles::Status).toString(), QString("Starting…"));
    QCOMPARE(placeholder()->data(SessionRoles::Group).toString(), destination);
    QVERIFY(!placeholder()->isHidden()); QVERIFY(!(placeholder()->flags() & Qt::ItemIsSelectable));
    QVERIFY(!(placeholder()->flags() & Qt::ItemIsDragEnabled));
    const auto preview = qEnvironmentVariable("HGS_NAVIGATION_PREVIEW");
    if (mode == "empty" && !preview.isEmpty()) { QDir().mkpath(preview); QVERIFY(window->grab().save(preview + "/starting-session.png")); }
    auto pending = QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object();
    QCOMPARE(pending.value(token).toObject().value("group").toString(), destination);
    QCOMPARE(SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object()).group(destination)->folders.size(),1);
    SessionInfo created; created.name = "codex/infra/grouped"; created.cmd = "codex"; created.project = "infra"; created.tag = "grouped";
    created.runId = "run-created"; created.state = "running"; created.launchId = "different-launch";
    auto box = host.isEmpty() ? state.local() : *state.peer(host);
    const QString machine = host.isEmpty() ? box.host : host;
    const auto deliver = [&] {
        box.sessions.removeIf([&](const SessionInfo &s) { return s.name == created.name; }); box.sessions.append(created);
        if (host.isEmpty()) state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); else state.setPeer(box, QDateTime::currentMSecsSinceEpoch());
        window->setFleet(state);
    };
    deliver(); // A matching name from someone else's launch is never moved.
    QVERIFY(placeholder());
    auto stored = SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(stored.groupFor(machine + '\n' + created.name), QString("ungrouped"));
    QVERIFY(QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().contains(token));
    if (mode == "failed") { window->newSessionLaunchFailed(token); QVERIFY(!placeholder()); }
    if (mode == "dismissed") {
        auto *list = window->findChild<SessionList *>("sessionList");
        list->customContextMenuRequested(list->visualItemRect(placeholder()).center());
        auto *menu = window->findChild<QMenu *>("pendingLaunchMenu"); QVERIFY(menu);
        menu->actions().first()->trigger(); menu->hide(); QVERIFY(!placeholder());
    }
    if (mode == "restart" || mode == "deleted" || mode == "legacy") {
        window.reset();
        if (mode == "legacy") {
            auto launch = pending.value(token).toObject(); launch.remove("show_pending"); pending[token] = launch;
            QSettings().setValue("workspace/pendingLaunches", QJsonDocument(pending).toJson(QJsonDocument::Compact));
        }
        if (mode == "deleted") {
            stored.removeGroup(group); QSettings().setValue("workspace/organization", QJsonDocument(stored.toJson()).toJson(QJsonDocument::Compact));
        }
        window = std::make_unique<SessionsWindow>(program()); window->setFleet(state); window->show();
        QCOMPARE(placeholder() != nullptr, mode != "legacy");
    }
    created.launchId = token; created.runId = "run-receipt"; deliver();
    QVERIFY(!placeholder());
    stored = SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(stored.groupFor(machine + '\n' + created.name), mode == "deleted" || mode == "failed" ? QString("ungrouped") : destination);
    QVERIFY(!QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().contains(token));
    const auto *resultGroup=stored.group(mode=="deleted"?stored.defaultProject():destination);QVERIFY(resultGroup);
    // The listed folder already belongs to the project, whatever path confirmed it.
    const bool added=std::any_of(resultGroup->folders.cbegin(),resultGroup->folders.cend(),[&](const SessionOrganization::Folder &f){return f.machine==machine&&f.path=="/remote/work tree";});
    QVERIFY(!added);
}

void TestSessionsWindow::launchPlaceholderReportsASessionThatEndedUnlisted_data()
{
    QTest::addColumn<QString>("host"); QTest::addColumn<QString>("mode");
    QTest::newRow("local-ended") << QString() << QString("ended");
    QTest::newRow("peer-ended") << QString("mac") << QString("ended");
    QTest::newRow("unknown-exit") << QString("mac") << QString("unknown");
    QTest::newRow("hidden-status") << QString() << QString("hidden");
    QTest::newRow("listed-launch") << QString("mac") << QString("listed");
    QTest::newRow("other-machine") << QString() << QString("other");
    QTest::newRow("unavailable") << QString() << QString("unavailable");
}

void TestSessionsWindow::launchPlaceholderReportsASessionThatEndedUnlisted()
{
    QFETCH(QString, host); QFETCH(QString, mode);
    // A detached launch reported started five minutes ago; no session or record appeared.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings().setValue("workspace/pendingLaunches", QJsonDocument(QJsonObject{{token, QJsonObject{{"host", host},
        {"cmd", "claude"}, {"tag", "failing"}, {"group", "ungrouped"}, {"target", "/remote/infra"}, {"add_folder", false},
        {"show_pending", mode != "hidden"}, {"created", QDateTime::currentSecsSinceEpoch() - 300}}}}).toJson(QJsonDocument::Compact));
    auto state = fleet();
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *list = window.findChild<SessionList *>("sessionList"); auto *notice = window.findChild<QLabel *>("notice");
    QVERIFY(list); QVERIFY(notice);
    auto placeholder = [&]() -> QListWidgetItem * {
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(SessionRoles::LaunchId).toString() == token) return list->item(i);
        return nullptr;
    };
    QCOMPARE(placeholder() != nullptr, mode != "hidden");
    if (placeholder()) QCOMPARE(placeholder()->data(SessionRoles::Status).toString(), QString("Waiting for session"));
    // The machine reports that the launch's session ended without a listed session or record.
    const QString reporter = mode != "other" ? host : host.isEmpty() ? QString("mac") : QString();
    auto box = reporter.isEmpty() ? state.local() : *state.peer(reporter);
    box.endedLaunches.insert(token, QJsonObject{{"launch_id", token}, {"name", "claude/infra/failing"},
        {"code", mode == "unknown" ? QJsonValue() : QJsonValue(3)},
        {"output", mode == "unknown" ? QString() : QString("hgs: tracking needs trusted hooks\ncannot start: example failure")}});
    if (mode == "listed") {
        // A session that carries the launch ID under another name is not an ended launch.
        SessionInfo renamed; renamed.name = "claude/infra/renamed"; renamed.cmd = "claude"; renamed.project = "infra";
        renamed.tag = "renamed"; renamed.state = "running"; renamed.launchId = token; box.sessions.append(renamed);
    }
    // A listing that reports its machine unavailable is no evidence.
    if (mode == "unavailable") box.ok = false;
    if (reporter.isEmpty()) state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    else state.setPeer(box, QDateTime::currentMSecsSinceEpoch());
    window.setFleet(state);
    const bool ends = mode != "listed" && mode != "other" && mode != "unavailable";
    QCOMPARE(placeholder() == nullptr, ends || mode == "hidden");
    QCOMPARE(QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().contains(token), !ends);
    if (mode == "ended") {
        QCOMPARE(notice->text(), QString("Session failing ended before it appeared (exit code 3).\n"
            "hgs: tracking needs trusted hooks\ncannot start: example failure"));
        QVERIFY(notice->isVisible()); QVERIFY(notice->property("error").toBool());
    } else if (mode == "unknown") {
        QCOMPARE(notice->text(), QString("Session failing ended before it appeared."));
    } else {
        // A hidden launch status stays hidden; a launch that may still appear keeps waiting.
        QVERIFY(!notice->text().contains("ended before it appeared"));
    }
    if (mode == "ended" || mode == "unknown") {
        // The next listing changes nothing: the launch is no longer pending.
        notice->clear(); window.setFleet(state);
        QVERIFY(notice->text().isEmpty()); QVERIFY(!placeholder());
    }
}

void TestSessionsWindow::endedLaunchOutranksALaterStartedCallback_data()
{
    QTest::addColumn<QString>("order");
    QTest::newRow("ended-then-started") << QString("ended");
    QTest::newRow("started-then-ended") << QString("started");
    QTest::newRow("listed-then-started") << QString("listed");
}

void TestSessionsWindow::endedLaunchOutranksALaterStartedCallback()
{
    QFETCH(QString, order);
    // A detached launch waits for both its command's result and its first listing.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings().setValue("workspace/pendingLaunches", QJsonDocument(QJsonObject{{token, QJsonObject{{"host", QString()},
        {"cmd", "claude"}, {"tag", "racing"}, {"group", "ungrouped"}, {"target", "/remote/infra"}, {"add_folder", false},
        {"show_pending", true}, {"created", QDateTime::currentSecsSinceEpoch()}}}}).toJson(QJsonDocument::Compact));
    auto state = fleet();
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *client = window.findChild<HgsClient *>(); auto *notice = window.findChild<QLabel *>("notice");
    auto *list = window.findChild<SessionList *>("sessionList");
    QVERIFY(client); QVERIFY(notice); QVERIFY(list);
    SessionInfo created; created.name = "claude/infra/racing"; created.cmd = "claude"; created.project = "infra";
    created.tag = "racing"; created.state = "running"; created.launchId = token;
    const auto deliver = [&](bool ended, bool listed) {
        auto box = state.local();
        if (ended) box.endedLaunches.insert(token, QJsonObject{{"launch_id", token}, {"name", created.name},
            {"code", 3}, {"output", "cannot start: example failure"}});
        if (listed) box.sessions.append(created);
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    };
    const auto opened = [&] {
        const auto *current = list->currentItem();
        return current && current->data(SessionRoles::Identity).toString() == "arch\n" + created.name;
    };
    if (order == "started") {
        client->nativeSessionLaunched(token, true, {});
        QCOMPARE(notice->text(), QString("Session started. Loading activity…"));
    }
    deliver(order != "listed", order == "listed");
    QVERIFY(!QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().contains(token));
    if (order != "started") client->nativeSessionLaunched(token, true, {});
    if (order == "listed") {
        // A placed launch still opens its session after a late success callback.
        QCOMPARE(notice->text(), QString("Session started. Loading activity…"));
        deliver(false, false); QVERIFY(opened());
        return;
    }
    // The ended report wins whichever arrives first.
    QCOMPARE(notice->text(), QString("Session racing ended before it appeared (exit code 3).\ncannot start: example failure"));
    QVERIFY(notice->isVisible()); QVERIFY(notice->property("error").toBool());
    // Nothing waits to open the ended launch: a later session with its ID is not selected.
    deliver(false, true); QVERIFY(!opened());
}

void TestSessionsWindow::endedLaunchOutranksALaterFailure_data()
{
    QTest::addColumn<QString>("order");
    QTest::newRow("ended-then-failed") << QString("ended");
    QTest::newRow("ended-then-terminal-failed") << QString("terminal");
    QTest::newRow("hidden-ended-then-failed") << QString("hidden");
    QTest::newRow("failed-then-ended") << QString("failed");
}

void TestSessionsWindow::endedLaunchOutranksALaterFailure()
{
    QFETCH(QString, order);
    // A non-detached launch command can fail after its session already ended.
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings().setValue("workspace/pendingLaunches", QJsonDocument(QJsonObject{{token, QJsonObject{{"host", QString()},
        {"cmd", "opencode"}, {"tag", "racing"}, {"group", "ungrouped"}, {"target", "/remote/infra"}, {"add_folder", false},
        {"show_pending", order != "hidden"}, {"created", QDateTime::currentSecsSinceEpoch()}}}}).toJson(QJsonDocument::Compact));
    auto state = fleet();
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *client = window.findChild<HgsClient *>(); auto *notice = window.findChild<QLabel *>("notice");
    QVERIFY(client); QVERIFY(notice);
    const auto end = [&] {
        auto box = state.local();
        box.endedLaunches.insert(token, QJsonObject{{"launch_id", token}, {"name", "opencode/infra/racing"},
            {"code", 3}, {"output", "cannot start: example failure"}});
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    };
    const QString ended("Session racing ended before it appeared (exit code 3).\ncannot start: example failure");
    if (order == "failed") {
        // A failure reported first ends the launch; the later listing adds nothing.
        client->nativeSessionLaunched(token, false, "timed out");
        QCOMPARE(notice->text(), QString("Could not start session racing: timed out"));
        end();
        QCOMPARE(notice->text(), QString("Could not start session racing: timed out"));
        QVERIFY(notice->isVisible()); QVERIFY(notice->property("error").toBool());
        return;
    }
    end();
    QVERIFY(!QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().contains(token));
    if (order == "hidden") QVERIFY(!notice->text().contains("ended before it appeared"));
    else QCOMPARE(notice->text(), ended);
    // A terminal launch reports its failure directly, possibly without a reason.
    if (order == "terminal") window.newSessionLaunchFailed(token);
    else client->nativeSessionLaunched(token, false, "timed out");
    // The ended report already carries the reason and its last lines. A hidden
    // launch status showed nothing, so its failure still names the launch.
    QCOMPARE(notice->text(), order == "hidden" ? QString("Could not start session racing: timed out") : ended);
    QVERIFY(notice->isVisible()); QVERIFY(notice->property("error").toBool());
}

void TestSessionsWindow::endedLaunchesInOneListingKeepUnshownFailures()
{
    // Pending launches are visited in key order; the last ended launch's
    // notice replaces the notices of those that ended in the same listing.
    const QString first("00000000-0000-4000-8000-000000000001"), second("00000000-0000-4000-8000-000000000002");
    QJsonObject pending;
    for (const auto &[token, tag] : {std::pair{first, QString("first")}, std::pair{second, QString("second")}})
        pending.insert(token, QJsonObject{{"host", QString()}, {"cmd", "opencode"}, {"tag", tag}, {"group", "ungrouped"},
            {"target", "/remote/infra"}, {"add_folder", false}, {"show_pending", true}, {"created", QDateTime::currentSecsSinceEpoch()}});
    QSettings().setValue("workspace/pendingLaunches", QJsonDocument(pending).toJson(QJsonDocument::Compact));
    auto state = fleet();
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *client = window.findChild<HgsClient *>(); auto *notice = window.findChild<QLabel *>("notice");
    QVERIFY(client); QVERIFY(notice);
    auto box = state.local();
    for (const auto &token : {first, second}) box.endedLaunches.insert(token, QJsonObject{{"launch_id", token}, {"code", 3}});
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    const QString ended("Session second ended before it appeared (exit code 3).");
    QCOMPARE(notice->text(), ended);
    // The shown launch keeps its notice; the other one never showed it, so its
    // later failure still names it.
    client->nativeSessionLaunched(second, false, "timed out");
    QCOMPARE(notice->text(), ended);
    client->nativeSessionLaunched(first, false, "timed out");
    QCOMPARE(notice->text(), QString("Could not start session first: timed out"));
    QVERIFY(notice->isVisible()); QVERIFY(notice->property("error").toBool());
}

void TestSessionsWindow::endedLaunchesWithoutAResultStayBounded()
{
    // Terminal launches and launches restored after a restart never report a
    // result, so the window remembers only the newest ended launches.
    constexpr int remembered = 16;
    QStringList tokens; QJsonObject pending;
    for (int i = 0; i <= remembered; ++i) {
        tokens << QUuid::createUuid().toString(QUuid::WithoutBraces);
        pending.insert(tokens.last(), QJsonObject{{"host", QString()}, {"cmd", "claude"}, {"tag", QString("ended-%1").arg(i)},
            {"group", "ungrouped"}, {"target", "/remote/infra"}, {"add_folder", false}, {"show_pending", true},
            {"created", QDateTime::currentSecsSinceEpoch()}});
    }
    QSettings().setValue("workspace/pendingLaunches", QJsonDocument(pending).toJson(QJsonDocument::Compact));
    auto state = fleet();
    SessionsWindow window(program()); window.setFleet(state); window.show();
    auto *client = window.findChild<HgsClient *>(); auto *notice = window.findChild<QLabel *>("notice");
    QVERIFY(client); QVERIFY(notice);
    auto box = state.local();
    for (const auto &token : tokens) {
        // Each listing reports one more launch ended, oldest first.
        box.endedLaunches.insert(token, QJsonObject{{"launch_id", token}, {"code", 3}});
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    }
    QVERIFY(QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().isEmpty());
    const QString newest = QString("Session ended-%1 ended before it appeared (exit code 3).").arg(remembered);
    QCOMPARE(notice->text(), newest);
    // The oldest remembered launch still keeps the ended notice over a late result.
    client->nativeSessionLaunched(tokens[1], true, {});
    QCOMPARE(notice->text(), newest);
    client->nativeSessionLaunched(tokens[2], false, "timed out");
    QCOMPARE(notice->text(), newest);
    // An older one was forgotten: its result is handled like that of any other launch.
    client->nativeSessionLaunched(tokens[0], true, {});
    QCOMPARE(notice->text(), QString("Session started. Loading activity…"));
}

void TestSessionsWindow::folderBrowserIgnoresStaleResponses()
{
    DirectoryDialog dialog(program(), "mac", "~"); dialog.show();
    auto *list = dialog.findChild<QListWidget *>("folderList"); QTRY_COMPARE(list->count(), 1);
    auto *client = dialog.findChild<HgsClient *>();
    client->directoriesReady(999, "mac", {{"path", "/stale"}, {"directories", QJsonArray{}}});
    QCOMPARE(list->count(), 1); QCOMPARE(dialog.directory(), QStringLiteral("/remote/work tree"));
    auto *path = dialog.findChild<QLineEdit *>("folderPath"); path->setFocus(); QTest::keyClicks(path, "/typed");
    QVERIFY(!dialog.findChild<QPushButton *>("chooseFolder")->isEnabled());
}

void TestSessionsWindow::legacyProjectsMigrateWithBackupAndLocalFolders()
{
    const auto legacy=QJsonDocument(QJsonObject{{"version",1},{"groups",QJsonArray{
        QJsonObject{{"id","work"},{"name","Work"},{"collapsed",true},{"sessions",QJsonArray{"mac\nclaude/infra/review"}}},
        QJsonObject{{"id","pinned"},{"name","Pinned"},{"sessions",QJsonArray{"arch\ncodex/hgs/dashboard"}}}}},
        {"before_pin",QJsonObject{{"arch\ncodex/hgs/dashboard","work"}}}}).toJson(QJsonDocument::Compact);
    QSettings().setValue("workspace/organization",legacy);
    SessionsWindow window(program());window.setFleet(fleet());
    auto *client=window.findChild<HgsClient *>("projectClient");QVERIFY(client);
    ProjectInfo local;local.name="Work";local.dir="/workspace/shared";local.exists=true;
    auto ignored=local;ignored.name="Ansible";ignored.fromAnsible=true;
    client->projectsForHostReady({}, {local,ignored});
    auto remote=local;remote.dir="/Users/test/shared";client->projectsForHostReady("mac",{remote});
    QCOMPARE(QSettings().value("workspace/organizationBeforeProjects").toByteArray(),legacy);
    SessionOrganization migrated(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(migrated.groups().size(),2);QVERIFY(!migrated.group("pinned"));
    QCOMPARE(migrated.groupFor("arch\ncodex/hgs/dashboard"),QString("work"));
    QCOMPARE(migrated.group("work")->folders.size(),2);QVERIFY(migrated.group("work")->collapsed);
    QCOMPARE(migrated.group("work")->folders[0].machine,QString("arch"));
    QCOMPARE(migrated.group("work")->folders[1].machine,QString("mac"));
    local.dir="/unexpected";client->projectsForHostReady({}, {local});
    SessionOrganization unchanged(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(unchanged.group("work")->folders.size(),2);
    auto offline=fleet();BoxState sleeping;sleeping.host="mac";offline.setPeer(sleeping,QDateTime::currentMSecsSinceEpoch());
    NewSessionDialog launch(program(),offline,"mac");launch.setGroups(migrated,"work");
    auto *start=launch.findChild<QPushButton *>("primary");QVERIFY(!start->isEnabled());
    launch.findChild<QLineEdit *>("launchName")->setText("preserved-name");launch.setFleet(fleet());
    QTRY_VERIFY(start->isEnabled());QCOMPARE(launch.findChild<QLineEdit *>("launchName")->text(),QString("preserved-name"));
    QCOMPARE(launch.findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+1).toString(),QString("mac"));
}

void TestSessionsWindow::projectOrderAndDefaultBadge()
{
    SessionOrganization projects;const auto first=projects.createGroup("infra"),second=projects.createGroup("sample-app");
    projects.addFolder(first,"arch","/workspace/infra");projects.moveSession(projects.observe("arch","codex/hgs/dashboard",{}),first);
    QSettings().setValue("workspace/hideEmptyProjects",false);
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    QSettings().setValue("workspace/theme","dark");
    SessionsWindow window(program());window.setFleet(fleet());window.show();window.showProjects();
    auto *page=window.findChild<ProjectsDialog *>();auto *list=page->findChild<QListWidget *>("logicalProjects");
    QCOMPARE(list->dragDropMode(),QAbstractItemView::InternalMove);QVERIFY(list->showDropIndicator());
    QCOMPARE(list->count(),3);QVERIFY(list->item(2)->data(Qt::UserRole+3).toBool());
    QVERIFY(list->item(2)->data(Qt::AccessibleTextRole).toString().contains("default project"));
    list->setCurrentRow(2);QVERIFY(list->model()->moveRow({},2,{},0));
    const auto stored=[] {return SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());};
    QTRY_COMPARE(stored().groups().first().id,QString("ungrouped"));
    QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),QString("ungrouped"));
    page->selectProject(first);page->findChild<QCheckBox *>("defaultProject")->click();
    QCOMPARE(stored().defaultProject(),first);QVERIFY(!list->item(0)->data(Qt::UserRole+3).toBool());
    QVERIFY(list->currentItem()->data(Qt::UserRole+3).toBool());
    page->findChild<QAction *>("moveProjectDown")->trigger();
    QCOMPARE(stored().groups().last().id,first);QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),first);
    QVERIFY(!page->findChild<QAction *>("moveProjectDown")->isEnabled());
    page->findChild<QAction *>("moveProjectUp")->trigger();QCOMPARE(stored().groups()[1].id,first);
    // Order and default are shared with Sessions and survive reopening the page.
    page->refresh();QCOMPARE(list->item(1)->data(Qt::UserRole).toString(),first);QVERIFY(list->item(1)->data(Qt::UserRole+3).toBool());
    window.showSessionList();auto *sessions=window.findChild<SessionList *>("sessionList");QStringList headers;
    for(int i=0;i<sessions->count();++i)if(sessions->item(i)->data(SessionRoles::Header).toBool())headers.append(sessions->item(i)->data(SessionRoles::Group).toString());
    QCOMPARE(headers,QStringList({"ungrouped",first,second}));QCOMPARE(stored().group(first)->folders.size(),1);
    const auto preview=qEnvironmentVariable("HGS_NAVIGATION_PREVIEW");
    if(!preview.isEmpty()){QDir().mkpath(preview);QTest::qWait(30);QVERIFY(page->grab().save(preview+"/projects-default-order.png"));}
}

void TestSessionsWindow::projectsEmbeddedAndRemote()
{
    SessionOrganization projects; const auto id=projects.createGroup("Shared infrastructure");
    projects.setColor(id,"#ffcc00");projects.addFolder(id,"arch","/workspace/infra","Backend");projects.addFolder(id,"mac","/Users/test/infra","Mac checkout");
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.showProjects("mac");
    auto *page=window.findChild<ProjectsDialog *>();QVERIFY(page);QVERIFY(!page->isWindow());page->selectProject(id);
    auto *table=page->findChild<QTableWidget *>("projectFolders");QCOMPARE(table->rowCount(),2);
    QCOMPARE(table->item(1,1)->text(),QString("mac"));QCOMPARE(table->item(1,2)->text(),QString("/Users/test/infra"));
    auto *name=page->findChild<QLineEdit *>("projectName");name->setText("Infrastructure / renamed");name->editingFinished();
    const auto restored=SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(restored.group(id)->name,QString("Infrastructure / renamed"));QCOMPARE(restored.group(id)->folders.size(),2);
    QCOMPARE(restored.group(id)->color,QString("#ffcc00"));
    NewSessionDialog launch(program(),fleet(),"mac");launch.setGroups(restored,id);
    QCOMPARE(launch.findChild<QComboBox *>("launchProjectFolder")->count(),1);
    auto *computer=launch.findChild<QComboBox *>("launchComputer");computer->setCurrentIndex(computer->findData(QString()));
    QCOMPARE(launch.findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+2).toString(),QString("/workspace/infra"));
    computer->setCurrentIndex(computer->findData("mac"));
    QCOMPARE(launch.findChild<QComboBox *>("launchProjectFolder")->currentData(Qt::UserRole+1).toString(),QString("mac"));
    for(auto *button:window.findChildren<QPushButton *>())if(button->property("filter")=="all")button->click();
    QVERIFY(!table->isVisible());
}

void TestSessionsWindow::projectFolderNewSessionPrefillsExactTarget()
{
    // The local folder is a real one of this computer, in the form of the GUI's platform.
    auto state=fleet();auto own=state.local();own.metrics.remove("platform");state.setLocal(own,QDateTime::currentMSecsSinceEpoch());
    const auto style=MachinePlatform::of(own.metrics,true);
    QTemporaryDir local;QVERIFY(local.isValid());
    SessionOrganization projects;const auto project=projects.createGroup("Chosen project");
    const auto localId=projects.addFolder(project,"arch",local.path(),"Local folder",style);
    projects.addFolder(project,"mac","/remote/other","Other remote folder");
    const auto remoteId=projects.addFolder(project,"mac","/remote/work tree","Chosen remote folder");
    projects.markImported("arch");projects.markImported("mac");
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program());window.resize(1150,760);window.setFleet(state);window.show();window.showProjects();
    auto *page=window.findChild<ProjectsDialog *>();page->selectProject(project);
    auto *table=page->findChild<QTableWidget *>("projectFolders");auto *create=page->findChild<QPushButton *>("newProjectFolderSession");
    const auto rowFor=[&](const QString &id){for(int row=0;row<table->rowCount();++row)if(table->item(row,0)->data(Qt::UserRole).toString()==id)return row;return -1;};
    QVERIFY(rowFor(localId)>=0);QVERIFY(rowFor(remoteId)>=0);
    QVERIFY(!create->isEnabled());table->setCurrentCell(rowFor(localId),0);QVERIFY(create->isEnabled());
    auto *metadata=page->findChild<QPushButton *>("projectColor");auto *defaultProject=page->findChild<QCheckBox *>("defaultProject");
    for(int height:{760,1100}) {
        window.resize(1150,height);table->clearSelection();QTest::qWait(30);
        QVERIFY(defaultProject->mapTo(page,QPoint()).y()-metadata->mapTo(page,QPoint(0,metadata->height())).y()<45);
        const int full=table->height();table->setCurrentCell(rowFor(localId),0);QTest::qWait(30);QVERIFY(full>=table->height());
        if(height==1100)QVERIFY(table->height()>230);
        auto *folderActions=page->findChild<QPushButton *>("addProjectFolder");
        QVERIFY(folderActions->mapTo(page,QPoint()).y()>=table->mapTo(page,QPoint(0,table->height())).y());
        auto *catalog=page->findChild<QTreeWidget *>("worktreeCatalog");auto *all=page->findChild<QPushButton *>("allWorktreeSessions");
        QVERIFY(all->mapTo(page,QPoint(0,all->height())).y()<=catalog->mapTo(page,QPoint()).y());
        // The compact heading keeps every worktree action visible without overlap.
        auto *panel=page->findChild<QWidget *>("worktreePanel");QWidget *previous=panel->findChild<QLabel *>("heading");
        for(const char *name:{"allWorktreeSessions","cleanUpWorktrees","refreshWorktrees","useWorktreeFolder"}){auto *button=panel->findChild<QPushButton *>(name);
            QVERIFY2(button->isVisible()&&previous->geometry().right()<button->geometry().left()&&button->geometry().right()<=panel->width(),name);previous=button;}
        if(const auto preview=qEnvironmentVariable("HGS_PROJECT_LAUNCH_PREVIEW");!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(page->grab().save(preview+QString("/projects-%1.png").arg(height)));}
        QVERIFY(defaultProject->mapTo(page,QPoint()).y()-metadata->mapTo(page,QPoint(0,metadata->height())).y()<45);
    }
    bool localOpened=false;
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<NewSessionDialog *>();QVERIFY(dialog);QTimer::singleShot(3000,dialog,&QDialog::reject);
        QCOMPARE(dialog->findChild<QComboBox *>("launchProject")->currentData().toString(),project);
        QCOMPARE(dialog->findChild<QComboBox *>("launchComputer")->currentData().toString(),QString());
        QCOMPARE(dialog->findChild<QComboBox *>("launchProjectFolder")->currentData().toString(),localId);
        QCOMPARE(dialog->findChild<QLabel *>("launchFolderPath")->text(),MachinePlatform::native(local.path(),style));localOpened=true;dialog->reject();});
    create->click();QVERIFY(localOpened);QVERIFY(page->isVisible());
    table->customContextMenuRequested(table->visualItemRect(table->item(rowFor(remoteId),0)).center());
    QPointer<QMenu> menu=page->findChild<QMenu *>("projectFolderMenu");QVERIFY(menu);auto *launch=menu->findChild<QAction *>("contextNewProjectFolderSession");
    QVERIFY(launch->isEnabled());table->setCurrentCell(rowFor(localId),0);
    QSignalSpy launched(&window,&SessionsWindow::newSessionRequested);bool remoteOpened=false;
    QTimer::singleShot(0,&window,[&]{auto *dialog=window.findChild<NewSessionDialog *>();QVERIFY(dialog);QTimer::singleShot(20000,dialog,&QDialog::reject);
        QCOMPARE(dialog->findChild<QComboBox *>("launchProject")->currentData().toString(),project);
        QCOMPARE(dialog->findChild<QComboBox *>("launchComputer")->currentData().toString(),QString("mac"));
        auto *folders=dialog->findChild<QComboBox *>("launchProjectFolder");QCOMPARE(folders->currentData().toString(),remoteId);
        window.setFleet(state);QCOMPARE(folders->currentData().toString(),remoteId);
        QCOMPARE(dialog->findChild<QLabel *>("launchFolderPath")->text(),QString("/remote/work tree"));remoteOpened=true;
        auto *accounts=dialog->findChild<QListWidget *>("launchAccount");
        const auto claude=[&]{for(int i=0;i<accounts->count();++i)if(accounts->item(i)->data(Qt::UserRole).toString()=="native-claude")return i;return -1;};
        QTRY_VERIFY_WITH_TIMEOUT(claude()>=0,15000);accounts->setCurrentRow(claude());dialog->findChild<QCheckBox *>("launchOpenTerminal")->setChecked(true);
        const auto preview=qEnvironmentVariable("HGS_PROJECT_LAUNCH_PREVIEW");if(!preview.isEmpty()){QDir().mkpath(preview);QTest::qWait(30);QVERIFY(dialog->grab().save(preview+"/launch.png"));}
        auto *start=dialog->findChild<QPushButton *>("primary");QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(),15000);start->click();});
    launch->trigger();QVERIFY(remoteOpened);QCOMPARE(launched.size(),1);QCOMPARE(launched[0][0].toString(),QString("mac"));
    QCOMPARE(launched[0][1].toString(),QString("claude"));QCOMPARE(launched[0][2].toString(),QString("/remote/work tree"));delete menu.data();
    window.showProjects();page->selectProject(project);table->setCurrentCell(rowFor(remoteId),0);
    table->customContextMenuRequested(table->visualItemRect(table->item(rowFor(remoteId),0)).center());menu=page->findChild<QMenu *>("projectFolderMenu");
    auto offline=state;BoxState missing;missing.host="mac";missing.error="offline";offline.setPeer(missing,QDateTime::currentMSecsSinceEpoch());window.setFleet(offline);
    QVERIFY(!create->isEnabled());QSignalSpy requests(page,&ProjectsDialog::newSessionRequested);
    menu->findChild<QAction *>("contextNewProjectFolderSession")->trigger();QCOMPARE(requests.size(),0);delete menu.data();
}

void TestSessionsWindow::projectFolderActionsKeepTargets()
{
    // Local folders are real ones of this computer, in the form of the GUI's platform;
    // the peer's folder is POSIX.
    auto state=fleet();auto own=state.local();own.metrics.remove("platform");state.setLocal(own,QDateTime::currentMSecsSinceEpoch());
    const auto style=MachinePlatform::of(own.metrics,true);
    QTemporaryDir temporary;QVERIFY(temporary.isValid());
    const auto path=temporary.path()+"/folder #1";QVERIFY(QDir().mkpath(path));const QString remote="/remote/folder #1";
    SessionOrganization projects;const auto first=projects.createGroup("First"),second=projects.createGroup("Second");
    projects.addFolder(first,"arch",path,"Local",style);projects.addFolder(first,"mac",remote,"Remote");
    projects.addFolder(first,"arch",path+"/missing","Missing",style);projects.addFolder(second,"arch",path,{},style);
    projects.markImported("arch");projects.markImported("mac");
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program());window.setFleet(state);window.show();window.showProjects();
    auto *page=window.findChild<ProjectsDialog *>();page->selectProject(first);
    auto *table=page->findChild<QTableWidget *>("projectFolders");
    auto *open=page->findChild<QPushButton *>("openProjectFolder");QVERIFY(open);QVERIFY(!open->isEnabled());
    FolderUrlRecorder files;table->setCurrentCell(0,0);QVERIFY(open->isEnabled());open->click();
    QCOMPARE(files.urls.size(),1);QCOMPARE(files.urls[0].toLocalFile(),path);
    const auto preview=qEnvironmentVariable("HGS_BULK_PREVIEW");
    if(!preview.isEmpty()){QDir().mkpath(preview);QTest::qWait(30);QVERIFY(page->grab().save(preview+"/project-folders.png"));}
    table->setCurrentCell(2,0);QCOMPARE(table->item(2,0)->text(),QString("Remote"));QVERIFY(!open->isEnabled());QVERIFY(open->toolTip().contains("mac"));
    table->customContextMenuRequested(table->visualItemRect(table->item(2,0)).center());
    QPointer<QMenu> menu=page->findChild<QMenu *>("projectFolderMenu");QVERIFY(menu);
    QVERIFY(!menu->findChild<QAction *>("contextOpenProjectFolder")->isEnabled());
    menu->findChild<QAction *>("contextCopyProjectFolder")->trigger();QCOMPARE(QApplication::clipboard()->text(),remote);
    table->setCurrentCell(0,0); // A menu command still edits the folder it was opened on.
    bool editedRemote=false;
    QTimer::singleShot(0,&window,[&]{auto *dialog=page->findChild<QDialog *>("projectFolderDialog");QVERIFY(dialog);
        editedRemote=dialog->findChild<QComboBox *>("projectFolderMachine")->currentData().toString()=="mac";
        QCOMPARE(dialog->findChild<QLineEdit *>("projectFolderPath")->text(),remote);dialog->reject();});
    menu->findChild<QAction *>("contextEditProjectFolder")->trigger();QVERIFY(editedRemote);delete menu.data();
    table->setCurrentCell(1,0);QCOMPARE(table->item(1,0)->text(),QString("Missing"));QVERIFY(!open->isEnabled());
    table->customContextMenuRequested(table->visualItemRect(table->item(0,0)).center());
    menu=page->findChild<QMenu *>("projectFolderMenu");QVERIFY(menu);
    menu->findChild<QAction *>("contextOpenProjectFolder")->trigger();QCOMPARE(files.urls.size(),2);
    page->selectProject(second);menu->findChild<QAction *>("contextRemoveProjectFolder")->trigger();
    QSignalSpy launches(page,&ProjectsDialog::newSessionRequested);menu->findChild<QAction *>("contextNewProjectFolderSession")->trigger();QCOMPARE(launches.size(),0);delete menu.data();
    const SessionOrganization saved(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(saved.group(first)->folders.size(),2);QCOMPARE(saved.group(second)->folders.size(),1);QVERIFY(QDir(path).exists());
    table->customContextMenuRequested(QPoint(5,table->viewport()->height()-5));
    menu=page->findChild<QMenu *>("projectFolderMenu");QVERIFY(menu);QCOMPARE(menu->actions().size(),1);
    QVERIFY(menu->findChild<QAction *>("contextAddProjectFolder"));delete menu.data();
    QTimer::singleShot(0,&window,[&]{auto *dialog=page->findChild<QDialog *>("deleteProjectDialog");QVERIFY(dialog);
        auto *buttons=dialog->findChild<QDialogButtonBox *>();QVERIFY(buttons);
        QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isDefault());
        QVERIFY(dialog->findChild<QLabel *>()->text().contains("First"));
        if(!preview.isEmpty()){QTest::qWait(30);QVERIFY(dialog->grab().save(preview+"/delete-project.png"));}
        QTest::keyClick(dialog,Qt::Key_Return);});
    page->deleteProject(first);
    const SessionOrganization afterDelete(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QVERIFY(afterDelete.group(first));
}

void TestSessionsWindow::localWindowsLaunchJoinsTheFolderInAnyCase()
{
    // A launch into a local Windows folder joins the project folder it names in any
    // case and with either separator (D15, D16); a new one is stored in Qt form.
    SessionOrganization projects;const auto project=projects.createGroup("Windows work");
    const auto existing=projects.addFolder(project,"arch","C:\\Work\\Repo","Repository",MachinePlatform::Style::Windows);
    projects.markImported("arch");projects.markImported("mac");
    QSettings().setValue("workspace/organization",QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    const QStringList tags{"same","new","rooted"};QStringList tokens;QJsonObject pending;
    const QList<QPair<QString,QString>> targets{{"c:\\work\\repo\\worktree","c:\\WORK\\repo"},{"D:\\New\\Folder","D:\\New\\Folder\\"},{"/repo","/repo"}};
    for(int i=0;i<tags.size();++i){
        tokens<<QUuid::createUuid().toString(QUuid::WithoutBraces);
        pending.insert(tokens[i],QJsonObject{{"host",""},{"cmd","codex"},{"tag",tags[i]},{"group",project},{"target",targets[i].first},
            {"add_folder",true},{"folder",targets[i].second},{"show_pending",true},{"created",QDateTime::currentSecsSinceEpoch()}});
    }
    QSettings().setValue("workspace/pendingLaunches",QJsonDocument(pending).toJson(QJsonDocument::Compact));
    auto state=fleet();auto local=state.local();local.metrics["platform"]="windows";
    for(int i=0;i<tags.size();++i){
        SessionInfo created;created.name="codex/work/"+tags[i];created.cmd="codex";created.project="work";created.tag=tags[i];
        created.runId="run-"+tags[i];created.state="running";created.launchId=tokens[i];local.sessions.append(created);
    }
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.show();window.setFleet(state);
    const SessionOrganization saved(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QStringList paths;for(const auto &folder:saved.group(project)->folders)paths<<folder.path;
    // A drive-less rooted path is no folder of a Windows machine: the session still joins.
    QCOMPARE(paths,QStringList({"C:/Work/Repo","D:/New/Folder"}));QCOMPARE(saved.group(project)->folders[0].id,existing);
    for(const auto &tag:tags)QCOMPARE(saved.groupFor("arch\ncodex/work/"+tag),project);
    QVERIFY(QJsonDocument::fromJson(QSettings().value("workspace/pendingLaunches").toByteArray()).object().isEmpty());
}

void TestSessionsWindow::localWindowsFolderFilterIgnoresCase()
{
    // A local Windows folder filter matches the CLI's paths in any case and with either
    // separator, and names the folder natively; a peer's POSIX folder stays exact.
    auto state=fleet();auto local=state.local();local.metrics["platform"]="windows";
    local.sessions[0].cwd=local.sessions[0].canonicalCwd="c:\\work\\repo";local.sessions[0].gitRoot="C:\\WORK\\Repo";
    local.sessions[1].cwd=local.sessions[1].canonicalCwd="C:\\Work\\Repo\\sub";local.sessions[1].gitRoot="C:\\Work\\Repo";
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    // A peer is POSIX even when it reports Windows (Phase 2 has no Windows peers).
    auto remote=*state.peer("mac");remote.metrics["platform"]="windows";remote.sessions[0].canonicalCwd=remote.sessions[0].gitRoot="/work/repo";
    state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.setFleet(state);window.resize(1200,850);window.show();
    auto *page=window.findChild<ProjectsDialog *>("projectsPage");auto *list=window.findChild<SessionList *>("sessionList");
    auto *clear=window.findChild<QPushButton *>("clearFolderFilter");
    const auto keys=[&]{QStringList result;for(int i=0;i<list->count();++i){const auto key=list->item(i)->data(SessionRoles::Key).toString();if(!key.isEmpty())result<<key;}result.sort();return result;};
    const auto all=[&]{for(auto *button:window.findChildren<QPushButton *>())if(button->property("filter")=="all")return button->property("count").toInt();return -1;};
    page->folderSessionsRequested({},"C:/Work/Repo",false);
    QCOMPARE(keys(),QStringList{"\ncodex/hgs/dashboard"});QCOMPARE(all(),1);
    QCOMPARE(clear->text(),QString("Folder: Repo   ×"));QVERIFY(clear->toolTip().startsWith("C:\\Work\\Repo\n"));
    page->folderSessionsRequested({},"c:/work/repo",true);
    QCOMPARE(keys(),QStringList({"\ncodex/hgs/dashboard","\nkimi/docs/research"}));QCOMPARE(all(),2);
    page->folderSessionsRequested("mac","/Work/Repo",true);QVERIFY(keys().isEmpty());QCOMPARE(all(),0);
    QCOMPARE(clear->text(),QString("Folder: Repo   ×"));QVERIFY(clear->toolTip().startsWith("/Work/Repo\n"));
    page->folderSessionsRequested("mac","/work/repo",true);QCOMPARE(keys(),QStringList{"mac\nclaude/infra/review"});QCOMPARE(all(),1);
    // A Linux computer keeps exact POSIX names.
    local.metrics["platform"]="linux";state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    page->folderSessionsRequested({},"c:/work/repo",true);QVERIFY(keys().isEmpty());QCOMPARE(all(),0);
    QVERIFY(clear->toolTip().startsWith("c:/work/repo\n"));
}

void TestSessionsWindow::projectColorsApplyOrCancel()
{
    SessionOrganization model;const auto id=model.createGroup("Release");model.setColor(id,"#00aa7f");
    QSettings().setValue("workspace/organization",QJsonDocument(model.toJson()).toJson(QJsonDocument::Compact));
    SessionsWindow window(program());window.setFleet(fleet());window.show();window.showProjects();
    auto *page=window.findChild<ProjectsDialog *>();page->selectProject(id);
    bool opened=false;
    const auto edit=[&](bool apply){
        opened=false;bool expired=false;QTimer watchdog;watchdog.setSingleShot(true);
        connect(&watchdog,&QTimer::timeout,&window,[&]{expired=true;if(auto *dialog=page->findChild<QDialog *>("projectColorDialog"))dialog->reject();});watchdog.start(5000);
        QTimer::singleShot(0,&window,[&]{
            auto *dialog=page->findChild<QDialog *>("projectColorDialog");QVERIFY(dialog);
            auto *hex=dialog->findChild<QLineEdit *>("projectColorHex");
            opened=hex&&dialog->findChild<QWidget *>("projectFillPreview");
            if(hex)hex->setText("#ffe100");
            const auto path=qEnvironmentVariable("HGS_PREVIEW_DIR");
            if(!path.isEmpty())dialog->grab().save(path+"/project-color-dialog.png");
            if(apply)dialog->accept();else dialog->reject();
        });
        page->findChild<QPushButton *>("projectColor")->click();
        watchdog.stop();QVERIFY(!expired);QVERIFY(opened);
    };
    edit(false);QVERIFY(opened);
    auto restored=SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(restored.group(id)->color,QString("#00aa7f"));
    edit(true);restored=SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object());
    QCOMPARE(restored.group(id)->color,QString("#ffe100"));QVERIFY(!restored.group(id)->vivid);
}

QTEST_MAIN(TestSessionsWindow)
#include "test_sessionswindow.moc"

void TestSessionsWindow::worktreeFilterKeepsConversationDraftAndSearchScope()
{
    auto state=fleet();auto local=state.local();local.sessions[0].cwd="/repo";local.sessions[0].canonicalCwd="/repo";local.sessions[0].gitRoot="/repo";
    local.sessions[1].cwd="/linked";local.sessions[1].canonicalCwd="/linked";local.sessions[1].gitRoot="/linked";state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    auto remote=*state.peer("mac");remote.sessions[0].cwd="/linked";remote.sessions[0].gitRoot="/linked";state.setPeer(remote,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.setFleet(state);window.resize(1200,850);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *composer=window.findChild<MessageComposer *>("messageComposer");setComposerText(composer, "Keep my draft");window.activateWindow();QTest::qWait(30);composer->editor()->setFocus();QTRY_COMPARE(QApplication::focusWidget(),composer->editor());
    auto *title=window.findChild<QLabel *>("detailTitle");const auto selected=title->text();
    auto *page=window.findChild<ProjectsDialog *>("projectsPage");page->folderSessionsRequested({},"/linked",true);
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->property("filter") == "all") QCOMPARE(button->property("count").toInt(), 1);
        if (button->property("filter") == "active") QCOMPARE(button->property("count").toInt(), 0);
    }
    auto *list=window.findChild<SessionList *>("sessionList");QStringList keys;
    for(int i=0;i<list->count();++i)if(!list->item(i)->data(SessionRoles::Key).toString().isEmpty())keys<<list->item(i)->data(SessionRoles::Key).toString();
    QCOMPARE(keys,QStringList{"\nkimi/docs/research"});QCOMPARE(title->text(),selected);QCOMPARE(composer->editor()->toPlainText(),QString("Keep my draft"));QCOMPARE(QApplication::focusWidget(),composer->editor());
    window.setFleet(state);QCOMPARE(title->text(),selected);QCOMPARE(composer->editor()->toPlainText(),QString("Keep my draft"));
    window.findChild<QLineEdit *>("search")->setText("research");QTest::qWait(300);
    auto *hits=window.findChild<QListWidget *>("searchResults");QVERIFY(hits->count()>0);for(int i=0;i<hits->count();++i)QVERIFY(hits->item(i)->data(Qt::AccessibleTextRole).toString().contains("research"));
    window.findChild<QLineEdit *>("search")->clear();window.findChild<QPushButton *>("clearFolderFilter")->click();QCOMPARE(title->text(),selected);QCOMPARE(composer->editor()->toPlainText(),QString("Keep my draft"));
    page->folderSessionsRequested({},"/empty",true);QCOMPARE(title->text(),selected);QCOMPARE(composer->editor()->toPlainText(),QString("Keep my draft"));
    for (auto *button : window.findChildren<QPushButton *>())
        if (button->property("filter").isValid()) QCOMPARE(button->property("count").toInt(), 0);
    page->relatedSessionRequested({},"kimi/docs/research",{});QVERIFY(title->text()!=selected);QVERIFY(!window.findChild<QPushButton *>("clearFolderFilter")->isVisible());
    window.showSession({},"codex/hgs/dashboard");QCOMPARE(composer->editor()->toPlainText(),QString("Keep my draft"));
    page->folderSessionsRequested({},"/empty",true);window.findChild<DashboardPage *>()->filterRequested("@all","all");QVERIFY(!window.findChild<QPushButton *>("clearFolderFilter")->isVisible());
}

void TestSessionsWindow::worktreePreview()
{
    const auto destination=qEnvironmentVariable("HGS_WORKTREE_PREVIEW");if(destination.isEmpty())QSKIP("Set HGS_WORKTREE_PREVIEW to write screenshots");QDir().mkpath(destination);
    const auto original=qApp->palette();
    for(bool dark:{true,false}){
        auto palette=original;palette.setColor(QPalette::Window,QColor(dark?"#161b21":"#f5f7f9"));palette.setColor(QPalette::Base,QColor(dark?"#1c2229":"#ffffff"));palette.setColor(QPalette::Text,QColor(dark?"#e8edf4":"#1a2733"));palette.setColor(QPalette::WindowText,palette.color(QPalette::Text));qApp->setPalette(palette);
        auto state=fleet();auto local=state.local();local.sessions[0].cwd="/repo";local.sessions[0].canonicalCwd="/repo";local.sessions[0].gitRoot="/repo";local.sessions[1].cwd="/linked";local.sessions[1].canonicalCwd="/linked";local.sessions[1].gitRoot="/linked";state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        SessionOrganization org;const auto project=org.createGroup("Worktree catalog");const auto rootId=org.addFolder(project,"arch","/repo","Core");
        for(const auto &name:{"Website","Backend","Android","Scripts","Docs","Tests"})org.addFolder(project,"arch","/workspace/"+QString(name).toLower(),name);
        org.markImported("arch");org.markImported("mac");QSettings().setValue("workspace/organization",QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
        SessionsWindow window(program());window.setFleet(state);window.resize(1260,900);window.show();window.showSession({},"codex/hgs/dashboard");
        window.findChild<QPushButton *>("toggleInspector")->setChecked(true);window.findChild<QTabWidget *>("sessionInspector")->setCurrentIndex(1);QTest::qWait(100);
        QVERIFY(window.grab().save(destination+(dark?"/details-dark.png":"/details-light.png")));
        window.findChild<QTabWidget *>("sessionInspector")->setCurrentIndex(2);QTest::qWait(100);
        QVERIFY(window.grab().save(destination+(dark?"/worktrees-dark.png":"/worktrees-light.png")));
        window.showProjects();auto *page=window.findChild<ProjectsDialog *>("projectsPage");page->selectProject(project);QTest::qWait(100);
        QVERIFY(window.grab().save(destination+(dark?"/projects-unselected-dark.png":"/projects-unselected-light.png")));
        auto *folders=page->findChild<QTableWidget *>("projectFolders");
        for(int row=0;row<folders->rowCount();++row)if(folders->item(row,0)->data(Qt::UserRole).toString()==rootId)folders->setCurrentCell(row,0);
        QTest::qWait(100);
        QVERIFY(window.grab().save(destination+(dark?"/projects-dark.png":"/projects-light.png")));
        window.resize(1260,760);QTest::qWait(100);QVERIFY(window.grab().save(destination+(dark?"/projects-short-dark.png":"/projects-short-light.png")));
        NewSessionDialog launch(program(),state,{},"codex",project,&window);launch.setGroups(org,project);launch.show();QTest::qWait(100);QVERIFY(launch.grab().save(destination+(dark?"/launch-dark.png":"/launch-light.png")));
        QTimer::singleShot(0,&launch,[&]{auto *form=launch.findChild<QDialog *>("newWorktreeDialog");QVERIFY(form);QTest::qWait(50);QVERIFY(form->grab().save(destination+(dark?"/create-dark.png":"/create-light.png")));form->reject();});launch.findChild<QAction *>("newLaunchWorktree")->trigger();
        QTimer::singleShot(0,&launch,[&]{auto *picker=launch.findChild<QDialog *>("chooseWorktreeDialog");QVERIFY(picker);QTest::qWait(100);QVERIFY(picker->grab().save(destination+(dark?"/picker-dark.png":"/picker-light.png")));picker->reject();});launch.findChild<QAction *>("chooseLaunchWorktree")->trigger();launch.reject();
    }
    qApp->setPalette(original);
}

void TestSessionsWindow::mobileConnectionSettingsOfferTheSharedDeliveryChoice(){
    SessionsWindow window(program()); window.show();
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *choice=window.findChild<QComboBox *>("mobileDelivery"); QVERIFY(choice); QCOMPARE(choice->count(),6);
    QVERIFY(window.findChild<QWidget *>("relaySettings"));
}

// hgs.exe on Windows reports a computer without sessions of its own; the
// workspace then manages remote machines only (driven by data, not by #ifdef).
void TestSessionsWindow::controlOnlyComputerOffersOnlyRemoteMachines(){
    auto state=fleet();auto local=state.local();local.sessions.clear();local.localSessions=false;
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.resize(1100,760);window.setFleet(state);window.show();
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *sections=window.findChild<QListWidget *>("settingsSections");QVERIFY(sections);
    QVERIFY(sections->item(5)->isHidden());QVERIFY(!sections->item(2)->isHidden());QVERIFY(!sections->item(4)->isHidden());
    // A Linux computer that runs sessions is a mobile gateway; without a platform
    // a Windows build would take this computer for Windows, which never is one.
    auto linuxFleet=fleet();auto linuxBox=linuxFleet.local();linuxBox.metrics=QJsonObject{{"platform","linux"}};
    linuxFleet.setLocal(linuxBox,QDateTime::currentMSecsSinceEpoch());
    window.setFleet(linuxFleet);QVERIFY(!sections->item(5)->isHidden());window.setFleet(state);
    NewSessionDialog launch(program(),state,{},"codex");launch.show();
    auto *machine=launch.findChild<QComboBox *>("launchComputer");QVERIFY(machine);
    QCOMPARE(machine->count(),1);QCOMPARE(machine->itemData(0).toString(),QString("mac"));
    FleetState alone;alone.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    NewSessionDialog lonely(program(),alone,{},"codex");lonely.show();
    QCOMPARE(lonely.findChild<QComboBox *>("launchComputer")->count(),0);
    QTRY_COMPARE(lonely.findChild<QLabel *>("launchError")->text(),QString("Add a remote machine in Machines to start sessions from this computer."));
    QVERIFY(!lonely.findChild<QPushButton *>("primary")->isEnabled());
    QVERIFY(!lonely.findChild<QPushButton *>("browseLaunchFolder")->isEnabled());
    QVERIFY(lonely.findChild<QPushButton *>("launchOpenMachines")->isVisible());
    QVERIFY(!launch.findChild<QPushButton *>("launchOpenMachines")->isVisible());
}

namespace {
// A Windows control console whose CLI reports a session host, alone: Machines
// offers Enable local sessions for it (spec D16).
FleetState enablableConsole()
{
    BoxState local;local.host="windows-pc";local.ok=true;local.localSessions=false;local.metrics={{"platform","windows"}};
    local.localHost={{"state","not-installed"},{"pid",QJsonValue::Null},{"protocol",QJsonValue::Null},{"conpty",QJsonValue::Null},
        {"integrity",QJsonValue::Null},{"windows_session",QJsonValue::Null}};
    FleetState fleet;fleet.setLocal(local,QDateTime::currentMSecsSinceEpoch());return fleet;
}
}

// Without remote machines a console that can enable local sessions names both
// ways, and New session only opens Machines with this computer selected.
void TestSessionsWindow::enablableConsoleNamesBothWaysToStartSessions(){
    const auto lonely=enablableConsole();
    SessionsWindow window(program());window.resize(1100,760);window.setFleet(lonely);window.show();
    bool checked=false;
    QTimer::singleShot(0,&window,[&]{
        auto *dialog=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget());QVERIFY(dialog);
        QTimer::singleShot(5000,dialog,&QDialog::reject);
        QCOMPARE(dialog->findChild<QComboBox *>("launchComputer")->count(),0);
        auto *error=dialog->findChild<QLabel *>("launchError");
        QTRY_COMPARE(error->text(),QString("Enable local sessions for this computer in Machines, or add a remote machine."));
        // The hint follows each poll: without a session host only a remote machine helps.
        auto plain=lonely;auto local=plain.local();local.localHost={};plain.setLocal(local,QDateTime::currentMSecsSinceEpoch());
        dialog->setFleet(plain);QCOMPARE(error->text(),QString("Add a remote machine in Machines to start sessions from this computer."));
        dialog->setFleet(lonely);QCOMPARE(error->text(),QString("Enable local sessions for this computer in Machines, or add a remote machine."));
        auto *open=dialog->findChild<QPushButton *>("launchOpenMachines");QVERIFY(open->isVisible());QVERIFY(open->isEnabled());
        if(const auto preview=qEnvironmentVariable("HGS_CONTROL_PREVIEW_DIR");!preview.isEmpty()){QDir().mkpath(preview);QVERIFY(dialog->grab().save(preview+"/launch-enable.png"));}
        checked=true;open->click();
    });
    window.findChild<QPushButton *>("newSession")->click();QVERIFY(checked);
    QVERIFY(!QApplication::activeModalWidget());
    auto *machines=window.findChild<QWidget *>("machinesPage");QTRY_VERIFY(machines->isVisible());
    QTRY_COMPARE(machines->findChild<QListWidget *>("machineProfiles")->currentItem()->data(Qt::UserRole).toString(),QString("@local"));
    // Only navigation: enabling stays behind its confirmation on the page.
    auto *enable=machines->findChild<QPushButton *>("enableLocalSessions");QVERIFY(enable->isVisible());
    QVERIFY(!machines->findChild<QMessageBox *>("enableLocalSessionsConfirm"));
}

void TestSessionsWindow::controlOnlyComputerKeepsRecoveryOnRemoteMachines(){
    QTemporaryDir fixture;QVERIFY(fixture.isValid());const auto script=fixture.filePath("hgs");
    QVERIFY(FakeCli::install(script,R"PY(#!/usr/bin/env python3
import json,sys
a=sys.argv[1:]
open(__file__+'.calls','a').write(json.dumps(a)+'\n')
if a[:2]!=['@mac','recovery']:sys.exit(2)
if a[2:3]==['set']:open(__file__+'.saved','wb').write(sys.stdin.buffer.read())
print('{"order":[7,"writer"],"policy":{"version":2,"revision":7,"writer":"writer","enabled":false,"enabled_at":0,"service":true,"network":true,"rate_limit":true,"delays":[15,30,60,300]}}')
)PY"));
    QSettings().remove("recovery/sharedPolicy");
    auto state=fleet();auto local=state.local();local.sessions.clear();local.localSessions=false;
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(script));window.resize(1100,900);window.setFleet(state);window.show();
    window.findChild<QPushButton *>("workspaceSettings")->click();auto &page=*static_cast<SettingsPage *>(window.findChild<QWidget *>("settingsPage"));page.openRecovery();
    auto *status=window.findChild<QLabel *>("recoverySyncStatus");QVERIFY(status);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Synchronized across 1 machines"),20000);
    auto *save=window.findChild<QPushButton *>("recoverySave");QTRY_VERIFY(save->isEnabled());
    window.findChild<QCheckBox *>("recoveryEnabled")->setChecked(true);save->click();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(script+".saved"),20000);
    // Recovery is never read from or written to this computer.
    QFile calls(script+".calls");QVERIFY(calls.open(QIODevice::ReadOnly));int recovery=0;
    for(const auto &line:calls.readAll().split('\n')){
        const auto call=QJsonDocument::fromJson(line).array();if(!call.contains(QJsonValue("recovery")))continue;
        ++recovery;QCOMPARE(call.first().toString(),QString("@mac"));
    }
    QVERIFY(recovery>=2);
}

void TestSessionsWindow::controlOnlyComputerAloneExplainsRecovery(){
    // A policy cached while remote machines were configured; none is left now.
    QSettings().setValue("recovery/sharedPolicy",QByteArray(R"({"order":[7,"writer"],"policy":{"version":2,"revision":7,"writer":"writer","enabled":true,"enabled_at":0,"service":true,"network":true,"rate_limit":true,"delays":[15,30,60,300]}})"));
    const auto cleanup=qScopeGuard([]{QSettings().remove("recovery/sharedPolicy");});
    FleetState alone;BoxState local;local.host="windows-pc";local.ok=true;local.localSessions=false;alone.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program());window.resize(1100,900);window.setFleet(alone);window.show();
    window.findChild<QPushButton *>("workspaceSettings")->click();static_cast<SettingsPage *>(window.findChild<QWidget *>("settingsPage"))->openRecovery();
    auto *status=window.findChild<QLabel *>("recoverySyncStatus");QVERIFY(status);
    QTRY_COMPARE(status->text(),QString("Add a remote machine in Machines to configure automatic recovery; this computer runs no agents."));
    QVERIFY(!status->text().contains("Synchronized"));
    QVERIFY(!window.findChild<QPushButton *>("recoverySave")->isEnabled());
    QVERIFY(!window.findChild<QCheckBox *>("recoveryEnabled")->isEnabled());
    QVERIFY(!window.findChild<QCheckBox *>("recoveryRule_service")->isEnabled());
    // Where Machines offers Enable local sessions, the status names it too.
    window.setFleet(enablableConsole());
    QTRY_COMPARE(status->text(),QString("To configure automatic recovery, enable local sessions for this computer in Machines or add a remote machine."));
    QVERIFY(!window.findChild<QPushButton *>("recoverySave")->isEnabled());
    window.setFleet(alone);QTRY_VERIFY(status->text().startsWith("Add a remote machine in Machines"));
}

// An enabled Windows computer runs sessions, so it keeps its recovery policy, but
// it is no mobile gateway (spec D16): Mobile connection stays hidden.
void TestSessionsWindow::enabledWindowsComputerIsNoMobileGateway(){
    QTemporaryDir fixture;QVERIFY(fixture.isValid());const auto script=fixture.filePath("hgs");
    QVERIFY(FakeCli::install(script,R"PY(#!/usr/bin/env python3
import sys
a=sys.argv[1:]
if a and a[0].startswith('@'):a=a[1:]
if a[:1]!=['recovery']:sys.exit(2)
print('{"order":[7,"writer"],"policy":{"version":2,"revision":7,"writer":"writer","enabled":false,"enabled_at":0,"service":true,"network":true,"rate_limit":true,"delays":[15,30,60,300]}}')
)PY"));
    QSettings().remove("recovery/sharedPolicy");
    auto state=fleet();auto local=state.local();local.metrics=QJsonObject{{"platform","windows"}};
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(FakeCli::program(script));window.resize(1100,900);window.setFleet(state);window.show();
    window.findChild<QPushButton *>("workspaceSettings")->click();
    auto *sections=window.findChild<QListWidget *>("settingsSections");QVERIFY(sections);
    QVERIFY(sections->item(5)->isHidden());QVERIFY(!sections->item(2)->isHidden());QVERIFY(!sections->item(4)->isHidden());
    // Recovery includes this computer as well as the remote machine.
    static_cast<SettingsPage *>(window.findChild<QWidget *>("settingsPage"))->openRecovery();
    auto *status=window.findChild<QLabel *>("recoverySyncStatus");QVERIFY(status);
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains("Synchronized across 2 machines"),20000);
    local.metrics=QJsonObject{{"platform","linux"}};state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    window.setFleet(state);QVERIFY(!sections->item(5)->isHidden());
    local.metrics=QJsonObject{{"platform","windows"}};state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    sections->setCurrentRow(5);window.setFleet(state);QVERIFY(sections->item(5)->isHidden());QCOMPARE(sections->currentRow(),0);
}

// Voice sends to local Windows sessions are a Phase 2 non-goal (spec D16): the
// disabled voice button says why.
void TestSessionsWindow::localWindowsSessionExplainsDisabledVoice(){
    const QJsonObject details{{"tracked",true},{"run_id","fixture-run"},{"conversation_id","fixture-conversation"},
        {"runtime_state","live"},{"process_state","running"},{"phase","idle"},{"activity","idle"},
        {"events",QJsonArray{}},{"cursor",0}};
    const auto fake=programInspecting("windows-voice",details);QVERIFY(!fake.isEmpty());
    auto state=fleet();auto local=state.local();local.metrics=QJsonObject{{"platform","linux"}};
    state.setLocal(local,QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(fake);window.setFleet(state);window.show();window.showSession({},"codex/hgs/dashboard");
    auto *client=window.findChild<HgsClient *>();auto *composer=window.findChild<MessageComposer *>("messageComposer");
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    settleSelectedInspection(composer,client,{},"codex/hgs/dashboard",details);
    auto *voice=composer->findChild<ToolbarChip *>("voiceRecord");QVERIFY(voice);
    QTRY_VERIFY(voice->isEnabled());QVERIFY(voice->toolTip().startsWith("Click to start"));
    local.metrics=QJsonObject{{"platform","windows"}};state.setLocal(local,QDateTime::currentMSecsSinceEpoch());window.setFleet(state);
    QTRY_VERIFY(!voice->isEnabled());
    QCOMPARE(voice->toolTip(),QString("Voice messages cannot be sent to sessions on this Windows computer yet."));
}

void TestSessionsWindow::fileCacheSettingsNavigation(){
    QSettings().setValue("downloads/folder", m_dir->filePath("Zerus Files"));
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.showUpdates();
    auto *sections = window.findChild<QListWidget *>("settingsSections"); QVERIFY(sections);
    QCOMPARE(sections->item(6)->text(), QString("Files"));
    QCOMPARE(sections->item(sections->count()-1)->text(), QString("About"));
    QCOMPARE(sections->item(sections->count()-2)->text(), QString("Keyboard"));
    QCOMPARE(sections->item(sections->count()-3)->text(), QString("Voice input"));
    sections->setCurrentRow(6);
    auto *files = window.findChild<DownloadCacheSettings *>("downloadCacheSettings"); QVERIFY(files); QVERIFY(files->isVisible());
    QCOMPARE(files->findChild<QComboBox *>("downloadCachePolicy")->currentText(), QString("Never clear"));
    QVERIFY(!files->findChild<QDoubleSpinBox *>("downloadCacheSize")->isEnabled());
    QTRY_VERIFY(files->findChild<QLabel *>("downloadCacheUsage")->text().contains("0 files"));
    if (qEnvironmentVariableIsSet("HGS_TEST_DOWNLOAD_SETTINGS_PREVIEW")) {
        files->findChild<QLineEdit *>("downloadCacheFolder")->setText("/Users/example/Downloads/Zerus Files");
        window.resize(1180, 820); QTest::qWait(80);
        QVERIFY(window.grab().save(qEnvironmentVariable("HGS_TEST_DOWNLOAD_SETTINGS_PREVIEW")));
    }
}

void TestSessionsWindow::automaticApprovalSettingsNavigation_data(){
    QTest::addColumn<QString>("theme");
    QTest::newRow("dark") << QString("dark"); QTest::newRow("light") << QString("light");
}

void TestSessionsWindow::automaticApprovalSettingsNavigation(){
    QFETCH(QString,theme); QSettings().setValue("workspace/theme",theme);
    QFile original(script()); QVERIFY(original.open(QIODevice::ReadOnly));
    const QByteArray journal = R"PY(elif c=='--help':print('hgs approval-log begin|finish|list|detail')
elif c=='approval-log':
 p=json.load(sys.stdin)
 if a[1]=='list':print(json.dumps(dict(entries=[dict(id=i,at=1792075200000+i*1000,host='desktop-a',session='claude/sample/review',prompt='Run this tool?',status=('unknown' if i==5 else 'approved')) for i in range(5,0,-1)],next=None,previous=None)))
 elif a[1]=='detail':print(json.dumps(dict(id=p['id'],status='unknown',question=dict(questions=[dict(id='approval',question='Run this tool?',body='printf fixture',options=[dict(id='yes',label='Yes')])]),answers=[dict(question_id='approval',selected_option_ids=['yes'])],detail='Delivery was not confirmed. Check Activity or Terminal.')))
)PY";
    const auto path=m_dir->filePath("approval-settings-"+theme);
    QVERIFY(FakeCli::install(path,original.readAll().replace("if False:pass\n","if False:pass\n"+journal)));
    SessionsWindow window(FakeCli::program(path)); window.setFleet(fleet()); window.resize(1280,900); window.show(); window.showUpdates();
    auto *sections=window.findChild<QListWidget *>("settingsSections"); QVERIFY(sections);
    QCOMPARE(sections->item(7)->text(),QString("Automatic approvals")); sections->setCurrentRow(7);
    auto *page=window.findChild<AutomaticApprovalSettings *>("automaticApprovalSettings"); QVERIFY(page); QVERIFY(page->isVisible());
    QVERIFY(!page->findChild<QCheckBox *>("automaticApprovalsEnabled")->isChecked());
    auto *table=page->findChild<QTableWidget *>("approvalJournal"); QTRY_COMPARE(table->rowCount(),5);
    table->setCurrentCell(0,0); auto *detail=page->findChild<QPlainTextEdit *>("approvalJournalDetail");
    QTRY_VERIFY(detail->toPlainText().contains("printf fixture")); QVERIFY(detail->toPlainText().contains("Selected answer: Yes"));
    if (qEnvironmentVariableIsSet("HGS_AUTOMATIC_APPROVALS_PREVIEW")) {
        QTest::qWait(80); QVERIFY(window.grab().save(qEnvironmentVariable("HGS_AUTOMATIC_APPROVALS_PREVIEW")+"-"+theme+".png"));
    }
}

void TestSessionsWindow::updatesEntryOpensExactSettingsPage(){
    SessionsWindow window(program());window.show();window.showUpdates();
    auto *sections=window.findChild<QListWidget *>("settingsSections");QVERIFY(sections);QCOMPARE(sections->currentRow(),4);QCOMPARE(sections->currentItem()->text(),QString("Updates"));
    QVERIFY(window.findChild<QWidget *>("settingsPage")->isVisible());window.showSessionList();window.showUpdates();QCOMPARE(sections->currentRow(),4);
}

namespace {
// Presses a chord on the platform input path: the window's QWindow first, then
// its focus widget and that widget's ancestors, as with a real keyboard.
void pressChord(QWidget *window, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QTest::keyClick(window->windowHandle(), key, modifiers);
}
// The physical Control key: Qt reports it as Meta on macOS, where Ctrl is Command.
Qt::KeyboardModifier physicalControl()
{
#ifdef Q_OS_MACOS
    return Qt::MetaModifier;
#else
    return Qt::ControlModifier;
#endif
}
// Presses a command's current chord (index: which one) key by key, sent to the
// window or, with a widget, to that widget. Shift+Tab arrives as Backtab, as
// from a real keyboard.
void pressSequence(QWidget *target, const QKeySequence &sequence, bool window = true)
{
    for (int i = 0; i < sequence.count(); ++i) {
        const auto modifiers = sequence[i].keyboardModifiers(); auto key = sequence[i].key();
        if (key == Qt::Key_Tab && (modifiers & Qt::ShiftModifier)) key = Qt::Key_Backtab;
        if (window) pressChord(target, key, modifiers); else QTest::keyClick(target, key, modifiers);
    }
}
void pressCommand(QWidget *window, const QString &id, int index = 0)
{
    const auto sequence = ShortcutRegistry::instance().keys(id).value(index);
    QVERIFY2(!sequence.isEmpty(), qPrintable(id));
    pressSequence(window, sequence);
}
QStackedWidget *workspacePages(QWidget &window)
{
    auto *dashboard = window.findChild<DashboardPage *>();
    return dashboard ? qobject_cast<QStackedWidget *>(dashboard->parentWidget()) : nullptr;
}
QPushButton *railButton(QWidget &window, const QString &glyph)
{
    for (auto *button : window.findChildren<QPushButton *>("railButton")) if (button->property("glyph") == glyph) return button;
    return nullptr;
}
QString currentSessionKey(const QListWidget *list)
{
    return list->currentItem() ? list->currentItem()->data(SessionRoles::Key).toString() : QString();
}
bool focusedIn(const QWidget *area)
{
    const auto *focus = QApplication::focusWidget();
    return focus && (focus == area || area->isAncestorOf(focus));
}

// A copy of the base fixture for Send and push now: inspect answers from
// <script>.details, send answers each message as the mode says, resume starts
// nothing, and send-now records each queue id in <script>.send-now.
struct PushFixture {
    enum class Send { Confirm, Slow, Refuse, Uncertain };
    QTemporaryDir dir; QString script = dir.filePath("hgs");
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
        {"process_state", "running"}, {"activity", "busy"}, {"phase", "working"}, {"events", QJsonArray{}}, {"cursor", 0}};
    // gateSendNow: send-now waits until release() before it answers.
    bool install(const QString &base, Send mode = Send::Confirm, bool gateSendNow = false)
    {
        QFile original(base); if (!original.open(QIODevice::ReadOnly)) return false;
        const QByteArray confirm = " print(json.dumps({'status':'submitted','request_id':payload['request_id'],'name':a[1],'run_id':payload['expected_run_id'],"
            "'conversation_id':payload['expected_conversation_id'],'first_message':not payload['expected_conversation_id']}))\n";
        // Every branch is a QByteArray: MSVC rejects a conditional that mixes literals with one.
        const QByteArray send = mode == Send::Refuse ? QByteArray(" print('refused by the fixture',file=sys.stderr);sys.exit(1)\n")
            : mode == Send::Uncertain ? QByteArray(" print('delivery uncertain: the fixture lost the reply',file=sys.stderr);sys.exit(1)\n")
            : mode == Send::Slow ? QByteArray(" __import__('time').sleep(3)\n") + confirm : confirm;
        return FakeCli::install(script, original.readAll().replace("if False:pass\n", "if False:pass\n"
            "elif c=='inspect':print(open(__file__+'.details').read())\n"
            "elif c=='resume':pass\n"
            "elif c=='send':\n payload=json.loads(sys.stdin.read())\n" + send +
            "elif c=='send-now':\n payload=json.loads(sys.stdin.read())\n"
            " open(__file__+'.send-now','a').write(payload['queue_id']+'\\n')\n"
            + QByteArray(gateSendNow ? " t=__import__('time').time()\n"
                " while not __import__('os').path.exists(__file__+'.release') and __import__('time').time()-t<30:__import__('time').sleep(0.05)\n" : "") +
            " print(json.dumps({'status':'submitted','request_id':payload['request_id'],'name':a[1],'run_id':payload['expected_run_id'],"
            "'conversation_id':payload['expected_conversation_id'],'queue_id':payload['queue_id']}))\n")) && save();
    }
    bool release() const { QFile file(script + ".release"); return file.open(QIODevice::WriteOnly); }
    void queue(const QString &id, const QString &text, bool canSendNow = true)
    {
        details["input_queue"] = QJsonObject{{"id", id}, {"text", text}, {"can_send_now", canSendNow}};
    }
    bool save() const { return FakeCli::replace(script + ".details", QJsonDocument(details).toJson(QJsonDocument::Compact)); }
    QStringList sentNow() const
    {
        QFile log(script + ".send-now");
        return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts) : QStringList();
    }
};

// Opens the dashboard session of a push fixture with its details settled.
void openPushSession(SessionsWindow &window, const FleetState &state, const PushFixture &fixture)
{
    keepDetailsPollsOut(window); window.setFleet(state); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    settleSelectedInspection(window.findChild<MessageComposer *>("messageComposer"), window.findChild<HgsClient *>(), {}, "codex/hgs/dashboard", fixture.details);
}

// Saves the fixture's details and waits until the window has read them.
void inspectPush(SessionsWindow &window, const PushFixture &fixture)
{
    QVERIFY(fixture.save());
    settleSelectedInspection(window.findChild<MessageComposer *>("messageComposer"), window.findChild<HgsClient *>(), {}, "codex/hgs/dashboard", fixture.details);
}

// Presses Send and push now in the message field.
void pressPush(SessionsWindow &window)
{
    auto *editor = window.findChild<MessageComposer *>("messageComposer")->editor();
    editor->setFocus(); QTRY_VERIFY(editor->hasFocus());
    const auto push = ShortcutRegistry::instance().keys("message.push").value(0); QCOMPARE(push.count(), 2);
    for (int i = 0; i < push.count(); ++i) pressChord(&window, push[i].key(), push[i].keyboardModifiers());
}

// Types text into the message field and presses Send and push now.
void pushMessage(SessionsWindow &window, const QString &text)
{
    setComposerText(window.findChild<MessageComposer *>("messageComposer"), text);
    pressPush(window);
}

bool pushes(const MessageComposer *composer)
{
    for (const auto *action : composer->editor()->actions()) if (action->objectName() == "shortcut:message.push") return true;
    return false;
}

QString notice(SessionsWindow &window) { return window.findChild<QLabel *>("notice")->text(); }
bool noticeIsError(SessionsWindow &window) { return window.findChild<QLabel *>("notice")->property("error").toBool(); }
}

void TestSessionsWindow::shortcutsSwitchSections()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *pages = workspacePages(window); QVERIFY(pages); QCOMPARE(pages->currentIndex(), 0);
    // Rail buttons in page order: Sessions, Projects, Machines, Overview, Accounts, Settings.
    const QList<QPushButton *> rail{railButton(window, "sessions"), railButton(window, "projects"), railButton(window, "machines"),
        window.findChild<QPushButton *>("brandMark"), railButton(window, "accounts"), window.findChild<QPushButton *>("workspaceSettings")};
    for (auto *button : rail) QVERIFY(button);
    QPushButton *attention = nullptr;
    for (auto *button : window.findChildren<QPushButton *>("sessionFilter")) if (button->property("filter") == "attention") attention = button;
    QVERIFY(attention); attention->click(); QVERIFY(attention->isChecked());
    const QList<QPair<QString, int>> chords{{"go.projects", 1}, {"go.machines", 2}, {"go.accounts", 4}, {"go.overview", 3},
        {"go.settings", 5}, {"go.sessions", 0}, {"go.sessions", 0}};
    for (const auto &chord : chords) {
        pressCommand(&window, chord.first);
        QCOMPARE(pages->currentIndex(), chord.second);
        for (int page = 0; page < rail.size(); ++page) QCOMPARE(rail[page]->isChecked(), page == chord.second);
    }
    // Unlike a second click on the rail button, the chord keeps the filter.
    QVERIFY(attention->isChecked());
}

void TestSessionsWindow::shortcutsMoveBetweenSessions()
{
    auto box = fleet().local(); auto first = box.sessions[0];
    first.activity = "idle"; first.phase = "idle"; first.currentTool.clear();
    first.subagents = {{"agent-a", QJsonObject{{"name", "Review"}, {"state", "working"}}}};
    box.sessions = {first, box.sessions[1]};
    FleetState state; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionOrganization org; const auto release = org.createGroup("Release"); org.createGroup("Research");
    for (const auto &name : {QStringLiteral("codex/hgs/dashboard"), QStringLiteral("kimi/docs/research")}) org.moveSession(org.observe("arch", name, {}), release);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    QSettings().setValue("workspace/expandedSessions", QStringList{QStringLiteral("\ncodex/hgs/dashboard")});
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    auto *list = window.findChild<SessionList *>("sessionList"); QVERIFY(list); QCOMPARE(list->count(), 4);
    QVERIFY(list->item(0)->data(SessionRoles::Header).toBool());
    QCOMPARE(list->item(2)->data(SessionRoles::ChildId).toString(), QString("agent-a"));
    QCOMPARE(list->currentRow(), 1);
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); setComposerText(composer, "Draft for the dashboard");
    // From the message field the chords switch sessions; each session keeps its own draft.
    composer->editor()->setFocus(); QTRY_VERIFY(composer->editor()->hasFocus());
    pressCommand(&window, "session.next");
    QCOMPARE(currentSessionKey(list), QString("\nkimi/docs/research")); QCOMPARE(composer->sessionKey(), QString("\nkimi/docs/research"));
    QTRY_VERIFY(!composer->isDraftLoading(composer->sessionKey()));
    QCOMPARE(composer->editor()->toPlainText(), QString()); QVERIFY(composer->editor()->hasFocus());
    pressCommand(&window, "session.previous");
    QCOMPARE(currentSessionKey(list), QString("\ncodex/hgs/dashboard"));
    QTRY_COMPARE(composer->editor()->toPlainText(), QString("Draft for the dashboard")); QVERIFY(composer->editor()->hasFocus());
    list->setFocus(); QTRY_VERIFY(list->hasFocus());
    pressCommand(&window, "session.next");
    QCOMPARE(currentSessionKey(list), QString("\nkimi/docs/research"));
    pressCommand(&window, "session.next"); // The last session stays: no wrap.
    QCOMPARE(currentSessionKey(list), QString("\nkimi/docs/research"));
    pressCommand(&window, "session.previous", 1); // The second chord: Ctrl+PgUp, or Command-Option-Up on macOS.
    QCOMPARE(currentSessionKey(list), QString("\ncodex/hgs/dashboard")); QCOMPARE(list->currentRow(), 1);
    pressCommand(&window, "session.previous"); // The project header is no session.
    QCOMPARE(list->currentRow(), 1);
    QTRY_COMPARE(composer->editor()->toPlainText(), QString("Draft for the dashboard"));
    // From another page the chord shows Sessions and moves from the selection.
    window.findChild<QPushButton *>("brandMark")->click(); QVERIFY(!list->isVisible());
    pressCommand(&window, "session.next", 1);
    QVERIFY(list->isVisible()); QCOMPARE(currentSessionKey(list), QString("\nkimi/docs/research"));
}

void TestSessionsWindow::shortcutsFindSessionsNeedingAttention()
{
    auto box = fleet().local(); const auto base = box.sessions[1]; box.sessions.clear();
    for (const auto &[name, phase] : QList<QPair<QString, QString>>{{"codex/alpha/one", "approval"}, {"codex/beta/two", "idle"}, {"codex/gamma/three", "input"}}) {
        auto session = base; session.name = name; session.cmd = "codex"; session.project = name.section('/', 1, 1); session.tag = name.section('/', 2);
        session.phase = phase; box.sessions << session;
    }
    FleetState state; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/gamma/three");
    auto *list = window.findChild<SessionList *>("sessionList"); list->setFocus(); QTRY_VERIFY(list->hasFocus());
    pressCommand(&window, "session.nextAttention"); // Wraps to the first.
    QCOMPARE(currentSessionKey(list), QString("\ncodex/alpha/one"));
    pressCommand(&window, "session.nextAttention"); // Skips the idle session.
    QCOMPARE(currentSessionKey(list), QString("\ncodex/gamma/three"));
    pressCommand(&window, "session.previousAttention");
    QCOMPARE(currentSessionKey(list), QString("\ncodex/alpha/one"));
    pressCommand(&window, "session.previousAttention"); // Wraps to the last.
    QCOMPARE(currentSessionKey(list), QString("\ncodex/gamma/three"));
    for (auto &session : box.sessions) session.phase = "idle";
    state.setLocal(box, QDateTime::currentMSecsSinceEpoch()); window.setFleet(state);
    pressCommand(&window, "session.nextAttention");
    QCOMPARE(currentSessionKey(list), QString("\ncodex/gamma/three"));
    QCOMPARE(window.findChild<QLabel *>("notice")->text(), QString("No sessions need attention"));
}

void TestSessionsWindow::shortcutsCycleSessionTabs()
{
    for (const bool processes : {false, true}) {
        QSettings().setValue("processes/enabled", processes);
        SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        window.showSession({}, "codex/hgs/dashboard");
        auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); auto *terminal = window.findChild<TerminalView *>();
        auto *processView = window.findChild<ProcessView *>(); QVERIFY(tabs); QVERIFY(terminal); QVERIFY(processView);
        QVERIFY(!tabs->isTabVisible(2)); // Native UI belongs to DeepSeek sessions.
        QCOMPARE(tabs->isTabVisible(tabs->indexOf(processView)), processes);
        QList<QWidget *> order{tabs->widget(0), terminal}; if (processes) order << processView;
        auto *composer = window.findChild<MessageComposer *>("messageComposer");
        composer->editor()->setFocus(); QTRY_VERIFY(composer->editor()->hasFocus());
        for (int step = 1; step <= order.size(); ++step) {
            pressCommand(&window, "tab.next");
            QCOMPARE(tabs->currentWidget(), order[step % order.size()]); QVERIFY(focusedIn(tabs->currentWidget()));
        }
        for (int step = order.size() - 1; step >= 0; --step) {
            pressCommand(&window, "tab.previous");
            QCOMPARE(tabs->currentWidget(), order[step]); QVERIFY(focusedIn(tabs->currentWidget()));
        }
    }
}

void TestSessionsWindow::terminalChordReturnsToActivity()
{
    // A Control+Shift+letter chord stays with the agent even where a command uses it.
    auto &registry = ShortcutRegistry::instance();
    const QKeyCombination shiftLetter(physicalControl() | Qt::ShiftModifier, Qt::Key_K);
    registry.setKeys("go.overview", registry.keys("go.overview") << QKeySequence(shiftLetter));
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); auto *terminal = window.findChild<TerminalView *>();
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); auto *list = window.findChild<SessionList *>("sessionList");
    auto *pages = workspacePages(window); auto *screen = terminal->terminalScreen();
    tabs->setCurrentWidget(terminal); screen->setFocus(); QTRY_VERIFY(screen->hasFocus());
    QSignalSpy output(screen, &TerminalScreen::output);
    // A chord the window takes from Terminal leaves the keys after it to the agent.
    pressCommand(&window, "zoom.reset");
    QCOMPARE(window.findChild<QLabel *>("notice")->text(), QString("Content scale 100%"));
    QCOMPARE(tabs->currentWidget(), terminal); QVERIFY(screen->hasFocus()); QCOMPARE(output.size(), 0);
    // Keys an agent needs stay in Terminal, bound or not: Control with letters
    // (Ctrl+R also refreshes on Linux and Windows) and with Shift and letters
    // other than copy and paste, Alt chords, function keys, Escape and Enter.
    const QList<QKeyCombination> agentKeys{QKeyCombination(physicalControl(), Qt::Key_C), QKeyCombination(physicalControl(), Qt::Key_R), shiftLetter,
        QKeyCombination(Qt::AltModifier, Qt::Key_Up), Qt::Key_F2, Qt::Key_Escape, Qt::Key_Return};
    for (const auto &key : agentKeys) {
        const auto before = output.size();
        pressChord(&window, key.key(), key.keyboardModifiers());
        QVERIFY2(output.size() > before, qPrintable(QKeySequence(key).toString()));
        QCOMPARE(tabs->currentWidget(), terminal); QCOMPARE(currentSessionKey(list), QString("\ncodex/hgs/dashboard"));
        QCOMPARE(pages->currentIndex(), 0); QVERIFY(!window.findChild<QDialog *>("renameSessionDialog"));
    }
    pressCommand(&window, "tab.terminal");
    QCOMPARE(tabs->currentIndex(), 0); QTRY_VERIFY(composer->editor()->hasFocus());
    pressCommand(&window, "tab.terminal");
    QCOMPARE(tabs->currentWidget(), terminal); QTRY_VERIFY(screen->hasFocus());
    pressCommand(&window, "go.overview");
    QCOMPARE(pages->currentIndex(), 3);
}

void TestSessionsWindow::shortcutsInModalDialogsStayLocal()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *pages = workspacePages(window); window.findChild<QPushButton *>("brandMark")->click(); QCOMPARE(pages->currentIndex(), 3);
    {
        QDialog dialog(&window); dialog.setModal(true); auto *field = new QLineEdit(&dialog);
        dialog.show(); dialog.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&dialog));
        field->setFocus(); QTRY_VERIFY(field->hasFocus());
        pressCommand(&dialog, "go.sessions");
        pressCommand(&dialog, "go.settings");
        QCOMPARE(pages->currentIndex(), 3);
    }
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    pressCommand(&window, "go.sessions"); QCOMPARE(pages->currentIndex(), 0);
}

void TestSessionsWindow::queueChordUsesTheExistingSendNowPath()
{
    QTemporaryDir fixture; QFile fake(fixture.filePath("hgs")), original(script());
    QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write(original.readAll().replace("if False:pass\n", "if False:pass\n"
        "elif c=='inspect':print(open(__file__+'.details').read())\n"
        "elif c=='send-now':\n payload=json.loads(sys.stdin.read())\n"
        " open(__file__+'.send-now','a').write(payload['queue_id']+'\\n')\n"
        " print(json.dumps({'status':'submitted','request_id':payload['request_id'],'name':a[1],'run_id':payload['expected_run_id'],"
        "'conversation_id':payload['expected_conversation_id'],'queue_id':payload['queue_id']}))\n"));
    fake.close(); QVERIFY(FakeCli::makeRunnable(fake.fileName()));
    QJsonObject details{{"tracked", true}, {"run_id", "run-one"}, {"conversation_id", "conversation-one"}, {"runtime_state", "live"},
        {"process_state", "running"}, {"activity", "busy"}, {"phase", "working"}, {"events", QJsonArray{}}, {"cursor", 0},
        {"input_queue", QJsonObject{{"id", "q1"}, {"text", "❯ run the release checks"}, {"can_send_now", true}, {"hint", "Interrupt and send the queue."}}}};
    const auto save = [&] { return FakeCli::replace(fake.fileName() + ".details", QJsonDocument(details).toJson(QJsonDocument::Compact)); };
    const auto requests = [&] {
        QFile log(fake.fileName() + ".send-now");
        return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()).split('\n', Qt::SkipEmptyParts) : QStringList();
    };
    QVERIFY(save());
    SessionsWindow window(FakeCli::program(fake.fileName())); keepDetailsPollsOut(window);
    window.setFleet(fleet()); window.show(); window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    settleSelectedInspection(composer, client, {}, "codex/hgs/dashboard", details);
    auto *sendNow = window.findChild<ActivityView *>("mainActivity")->findChild<QPushButton *>("queueSendNow");
    QTRY_VERIFY(sendNow->isVisible()); QVERIFY(sendNow->isEnabled());
    QCOMPARE(sendNow->toolTip(), QString("Interrupt and send the queue. (%1)").arg(ShortcutRegistry::instance().hint("queue.sendNow")));
    setComposerText(composer, "Typed but not sent"); composer->editor()->setFocus(); QTRY_VERIFY(composer->editor()->hasFocus());
    QSignalSpy sent(composer, &MessageComposer::sendRequested); QSignalSpy finished(client, &HgsClient::queueSendFinished);
    pressCommand(&window, "queue.sendNow");
    QTRY_COMPARE(finished.size(), 1); QVERIFY(finished[0][1].toBool());
    QCOMPARE(requests(), QStringList{"q1"}); QCOMPARE(sent.size(), 0);
    QCOMPARE(composer->editor()->toPlainText(), QString("Typed but not sent"));
    // A queue that cannot be sent now sends nothing and says so.
    details["input_queue"] = QJsonObject{{"id", "q2"}, {"text", "❯ later"}, {"can_send_now", false}};
    QVERIFY(save()); settleSelectedInspection(composer, client, {}, "codex/hgs/dashboard", details);
    pressCommand(&window, "queue.sendNow");
    QCOMPARE(window.findChild<QLabel *>("notice")->text(), QString("Nothing queued can be sent now"));
    QTest::qWait(150); QCOMPARE(requests(), QStringList{"q1"}); QCOMPARE(sent.size(), 0);
}

void TestSessionsWindow::zoomChordsChangeContentScale()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); composer->editor()->setFocus(); QTRY_VERIFY(composer->editor()->hasFocus());
    auto *slider = window.findChild<QSlider *>("workspaceContentScale"); auto *value = window.findChild<QLabel *>("workspaceContentScaleValue");
    auto *notice = window.findChild<QLabel *>("notice"); QVERIFY(slider); QVERIFY(value);
    const auto editorPixels = [composer] { return QFontInfo(composer->editor()->font()).pixelSize(); };
    QCOMPARE(editorPixels(), 13);
    pressCommand(&window, "zoom.in");
    QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 1.1);
    QCOMPARE(slider->value(), 22); QCOMPARE(value->text(), QString("110%")); QCOMPARE(notice->text(), QString("Content scale 110%"));
    QTRY_COMPARE(editorPixels(), 14);
    pressCommand(&window, "zoom.reset");
    QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 1.0);
    QCOMPARE(slider->value(), 20); QCOMPARE(value->text(), QString("100%")); QTRY_COMPARE(editorPixels(), 13);
    for (int i = 0; i < 4; ++i) pressCommand(&window, "zoom.out");
    QCOMPARE(QSettings().value("workspace/contentScale").toDouble(), 0.75); QCOMPARE(notice->text(), QString("Content scale 75%"));
}

void TestSessionsWindow::reboundChordsApplyAtOnce()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    const auto native = [](const char *chord) { return QKeySequence::fromString(chord, QKeySequence::PortableText).toString(QKeySequence::NativeText); };
    auto *projects = railButton(window, "projects"); QVERIFY(projects);
    QCOMPARE(projects->toolTip(), QString("Projects (%1)").arg(native("Ctrl+3")));
    QCOMPARE(window.findChild<QPushButton *>("newSession")->toolTip(), QString("New session (%1)").arg(native("Ctrl+N")));
    auto *inspector = window.findChild<QPushButton *>("toggleInspector"); QVERIFY(inspector->toolTip().endsWith(" (" + native("Ctrl+I") + ")"));
    pressCommand(&window, "panel.toggle"); QVERIFY(inspector->isChecked());
    QCOMPARE(inspector->toolTip(), QString("Collapse side panel (%1)").arg(native("Ctrl+I")));
    QCOMPARE(inspector->accessibleName(), QString("Collapse side panel"));
    auto *tabs = window.findChild<QTabWidget *>("sessionDetailTabs"); auto *terminal = window.findChild<TerminalView *>();
    tabs->setCurrentWidget(terminal); terminal->terminalScreen()->setFocus(); QTRY_VERIFY(terminal->terminalScreen()->hasFocus());
    const auto previousTab = ShortcutRegistry::instance().keys("tab.next").value(0); QVERIFY(!previousTab.isEmpty());
    const auto portable = [](const char *chord) { return QKeySequence::fromString(chord, QKeySequence::PortableText); };
    ShortcutRegistry::instance().setKeys("tab.next", {portable("Ctrl+9")});
    ShortcutRegistry::instance().setKeys("go.projects", {portable("Ctrl+8")});
    QCOMPARE(window.findChild<QAction *>("shortcut:tab.next")->shortcuts(), QList<QKeySequence>{portable("Ctrl+9")});
    QCOMPARE(projects->toolTip(), QString("Projects (%1)").arg(native("Ctrl+8")));
    pressSequence(&window, previousTab); // No longer bound: Terminal keeps it.
    QCOMPARE(tabs->currentWidget(), terminal);
    pressCommand(&window, "tab.next");
    QCOMPARE(tabs->currentIndex(), 0);
}

void TestSessionsWindow::projectListKeepsItsReorderKeys()
{
    SessionOrganization projects; projects.createGroup("infra"); projects.createGroup("sample-app");
    QSettings().setValue("workspace/hideEmptyProjects", false);
    QSettings().setValue("workspace/organization", QJsonDocument(projects.toJson()).toJson(QJsonDocument::Compact));
    const auto stored = [] { return SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object()); };
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showProjects();
    auto *pages = workspacePages(window); auto *page = window.findChild<ProjectsDialog *>();
    auto *list = page->findChild<QListWidget *>("logicalProjects"); QVERIFY(list); QVERIFY(list->count() > 2);
    list->setCurrentRow(1); list->setFocus(); QTRY_VERIFY(list->hasFocus());
    const auto moved = stored().groups()[1].id;
    // Alt+Up and Alt+Down are the list's own keys, although Next and Previous session use them elsewhere.
    QCOMPARE(ShortcutRegistry::instance().keys("session.next").value(0), QKeySequence(Qt::ALT | Qt::Key_Down));
    pressChord(&window, Qt::Key_Up, Qt::AltModifier);
    QCOMPARE(stored().groups()[0].id, moved); QCOMPARE(pages->currentIndex(), 1);
    list->setCurrentRow(list->count() - 1);
    pressChord(&window, Qt::Key_Down, Qt::AltModifier); // The last project stays last.
    QCOMPARE(pages->currentIndex(), 1);
    auto *other = page->findChild<QCheckBox *>("allSwarmProjects"); other->setFocus(); QTRY_VERIFY(other->hasFocus());
    pressCommand(&window, "session.next");
    QCOMPARE(pages->currentIndex(), 0);
}

void TestSessionsWindow::focusChordsCycleSessionAreas()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    auto *list = window.findChild<SessionList *>("sessionList"); auto *activity = window.findChild<ActivityView *>("mainActivity");
    auto *composer = window.findChild<MessageComposer *>("messageComposer"); auto *panel = window.findChild<QFrame *>("sessionInspectorPanel");
    pressCommand(&window, "panel.toggle"); QTRY_VERIFY(panel->isVisible());
    list->setFocus(); QTRY_VERIFY(list->hasFocus());
    // Session list, the current view, the message field, the side panel, and around.
    const QList<const QWidget *> areas{list, activity->browser(), composer->editor(), panel};
    for (int step = 1; step <= areas.size(); ++step) {
        pressCommand(&window, "focus.next");
        QVERIFY2(focusedIn(areas[step % areas.size()]), qPrintable(QString::number(step)));
    }
    for (int step = areas.size() - 1; step >= 0; --step) {
        pressCommand(&window, "focus.previous");
        QVERIFY2(focusedIn(areas[step]), qPrintable(QString::number(step)));
    }
    // Other pages have no such areas.
    window.findChild<QPushButton *>("brandMark")->click(); auto *focus = QApplication::focusWidget();
    pressCommand(&window, "focus.next"); QCOMPARE(QApplication::focusWidget(), focus);
}

void TestSessionsWindow::goToOpensAndActivatesItems()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *pages = workspacePages(window); QVERIFY(pages);
    auto *sessions = window.findChild<SessionList *>(); QVERIFY(sessions);
    pressCommand(&window, "go.anywhere");
    auto *goTo = window.findChild<GoToDialog *>("goToDialog"); QVERIFY(goTo);
    QTRY_VERIFY(goTo->isVisible());
    auto *results = goTo->findChild<QListWidget *>("goToResults"); QVERIFY(results);
    QStringList titles; QList<GoToItem> items;
    for (int row = 0; row < results->count(); ++row) {
        const auto item = results->item(row)->data(Qt::UserRole).value<GoToItem>();
        items << item; titles << item.title;
    }
    QVERIFY(titles.contains("hgs / dashboard"));
    QVERIFY(titles.contains("infra / review"));
    QVERIFY(titles.contains("Next session"));
    QVERIFY(titles.contains("Projects"));
    QVERIFY(titles.contains("mac"));
    // Message-field commands and Go to itself do not apply from the popup.
    QVERIFY(!titles.contains("Send message"));
    QVERIFY(!titles.contains("Go to…"));
    const auto find = [&](GoToItem::Kind kind, const QString &title) {
        for (const auto &item : items) if (item.kind == kind && item.title == title) return item;
        return GoToItem{};
    };
    goTo->hide();
    // Hiding the popup directly does not hand activation back on every
    // platform plugin (macOS offscreen); window chords need the active window.
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    // A session on another computer opens and becomes the selection.
    const auto review = find(GoToItem::Session, "infra / review"); QCOMPARE(review.id, QString("mac\nclaude/infra/review"));
    emit goTo->activated(review);
    QCOMPARE(pages->currentIndex(), 0);
    QCOMPARE(currentSessionKey(sessions), review.id);
    // A section and a command run like their chords.
    emit goTo->activated(find(GoToItem::Section, "Projects"));
    QCOMPARE(pages->currentIndex(), 1);
    emit goTo->activated(find(GoToItem::Section, "Sessions"));
    auto *toggle = window.findChild<QPushButton *>("toggleInspector"); QVERIFY(toggle);
    const bool panel = toggle->isChecked();
    emit goTo->activated(find(GoToItem::Command, "Show or hide the side panel"));
    QCOMPARE(toggle->isChecked(), !panel);
    // A project without sessions opens Projects.
    GoToItem empty; empty.kind = GoToItem::Project; empty.id = "no-such-project";
    emit goTo->activated(empty);
    QCOMPARE(pages->currentIndex(), 1);
    // Recently opened sessions come first with an empty query.
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    pressCommand(&window, "go.sessions");
    pressCommand(&window, "go.anywhere");
    QTRY_VERIFY(goTo->isVisible());
    QVERIFY(results->count() > 0);
    QCOMPARE(results->item(0)->data(Qt::UserRole).value<GoToItem>().id, review.id);
    // Go to's own chord closes the popup again.
    pressSequence(goTo->findChild<QLineEdit *>("goToQuery"), ShortcutRegistry::instance().keys("go.anywhere").value(0), false);
    QTRY_VERIFY(!goTo->isVisible());
}

void TestSessionsWindow::cheatSheetOpensKeyboardSettings()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    pressCommand(&window, "help.shortcuts");
    auto *help = window.findChild<ShortcutsDialog *>("shortcutsDialog"); QVERIFY(help);
    QTRY_VERIFY(help->isVisible());
    auto *customize = help->findChild<QPushButton *>("shortcutsCustomize"); QVERIFY(customize);
    customize->click();
    QVERIFY(!help->isVisible());
    auto *pages = workspacePages(window); QVERIFY(pages); QCOMPARE(pages->currentIndex(), 5);
    auto *sections = window.findChild<QListWidget *>("settingsSections"); QVERIFY(sections);
    QCOMPARE(sections->currentItem()->text(), QString("Keyboard"));
}

// Push tests wait for deliveries through the fake CLI, which is slow on Windows runners.
constexpr int PushTimeout = 20000;

void TestSessionsWindow::pushSendsNowWhenTheMessageIsQueued()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    // Only the main message field pushes: subagent messages do not enter the session's queue.
    QVERIFY(pushes(composer)); QVERIFY(!pushes(window.findChild<MessageComposer *>("subagentComposer")));
    QSignalSpy delivered(client, &HgsClient::messageSent), queued(client, &HgsClient::queueSendFinished);
    pushMessage(window, "Run the release checks before tagging v2"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    // The agent queued it; its terminal wraps the text.
    fixture.queue("q-push", "❯ Run the release checks\n  before tagging v2"); inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-push"}, PushTimeout); QTRY_COMPARE(queued.size(), 1);
    // The push is spent: it never resends text or the queue.
    inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList{"q-push"}); QCOMPARE(delivered.size(), 1);
    // With an empty field the chord sends the existing queue now.
    QCOMPARE(composer->editor()->toPlainText(), QString());
    fixture.queue("q-later", "❯ later"); inspectPush(window, fixture);
    pushMessage(window, {});
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), (QStringList{"q-push", "q-later"}), PushTimeout); QCOMPARE(delivered.size(), 1);
}

void TestSessionsWindow::pushIgnoresQueuesWithoutTheMessage()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "Summarize the failing tests in one table"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    fixture.queue("q-other", "❯ Rename the release branch"); inspectPush(window, fixture);
    fixture.queue("q-blocked", "❯ Summarize the failing tests in one table", false); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
    fixture.queue("q-match", "❯ Rename the release branch\n❯ Summarize the   failing tests in one table"); inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-match"}, PushTimeout);
}

void TestSessionsWindow::pushIgnoresTheMessageInsideAnotherWord()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "test suite on CI"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    // Only longer words hold the message: "latest" ends with "test", "CI-only" begins with "CI".
    fixture.queue("q-latest", "❯ Rerun the latest suite on CI"); inspectPush(window, fixture);
    fixture.queue("q-longer", "❯ test suite on CI-only runners"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
    fixture.queue("q-match", "❯ Rerun the latest suite on CI\n❯ test suite on CI"); inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-match"}, PushTimeout);
}

void TestSessionsWindow::pushDropsAfterSessionIdentityChange_data()
{
    QTest::addColumn<QString>("field"); QTest::addColumn<QString>("value");
    QTest::newRow("run") << QStringLiteral("run_id") << QStringLiteral("run-two");
    QTest::newRow("conversation") << QStringLiteral("conversation_id") << QStringLiteral("conversation-two");
}

void TestSessionsWindow::pushDropsAfterSessionIdentityChange()
{
    QFETCH(QString, field); QFETCH(QString, value);
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "Check the deployment logs again"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    fixture.details[field] = value; fixture.queue("q-changed", "❯ Check the deployment logs again"); inspectPush(window, fixture);
    QTRY_COMPARE(notice(window), QString("The message was not pushed: the session changed")); QVERIFY(noticeIsError(window));
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
}

void TestSessionsWindow::pushFollowsTheRunThatTakesTheMessage()
{
    PushFixture fixture;
    fixture.details = QJsonObject{{"tracked", true}, {"run_id", "run-old"}, {"conversation_id", "conversation-old"},
        {"runtime_state", "saved"}, {"process_state", "exited"}, {"events", QJsonArray{}}, {"cursor", 0}};
    QVERIFY(fixture.install(script()));
    auto state = fleet(); auto box = state.local();
    const auto setSession = [&](const QString &value, const QString &run, const QString &conversation) {
        for (auto &session : box.sessions) if (session.name == "codex/hgs/dashboard") {
            session.state = value; session.runId = run; session.conversationId = conversation; session.currentTool.clear();
            session.processState = value == "running" ? "running" : "exited"; session.activity = "idle"; session.phase = "idle";
        }
        state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    };
    setSession("stopped", "run-old", "conversation-old");
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, state, fixture);
    if (QTest::currentTestFailed()) return;
    auto *client = window.findChild<HgsClient *>();
    QSignalSpy delivered(client, &HgsClient::messageSent), writes(client, &HgsClient::writeDone);
    // The stopped session starts fresh before it takes the message: the push waits for that run.
    QTimer answer; answer.setInterval(10);
    connect(&answer, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QMessageBox *>("startToSendConfirm");
        if (!dialog || !dialog->isVisible()) return;
        answer.stop(); dialog->findChild<QPushButton *>("startFreshAndSend")->click();
    });
    answer.start();
    pushMessage(window, "Start over and outline the plan"); if (QTest::currentTestFailed()) return;
    QVERIFY(!answer.isActive());
    QTRY_COMPARE(writes.size(), 1);
    setSession("running", "run-new", {}); window.setFleet(state);
    fixture.details = QJsonObject{{"tracked", true}, {"run_id", "run-new"}, {"runtime_state", "live"}, {"process_state", "running"},
        {"activity", "idle"}, {"phase", "idle"}, {"first_message_can_send", true}, {"events", QJsonArray{}}, {"cursor", 0}};
    inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout); QCOMPARE(delivered[0][3].toJsonObject()["run_id"].toString(), QString("run-new"));
    // The message began the run's first conversation, which the push adopts.
    fixture.details["conversation_id"] = "conversation-new"; fixture.details["activity"] = "busy"; fixture.details["phase"] = "working";
    fixture.queue("q-fresh", "❯ Start over and outline the plan"); inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-fresh"}, PushTimeout);
}

void TestSessionsWindow::pushStaysWithItsSession()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    QSignalSpy delivered(client, &HgsClient::messageSent);
    pushMessage(window, "Draft the release notes for v2"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    // Its session is no longer inspected once another one is selected: the push ends at once.
    fixture.queue("q-other-session", "❯ Draft the release notes for v2"); QVERIFY(fixture.save());
    window.showSession({}, "kimi/docs/research");
    QCOMPARE(notice(window), QString("The message to hgs / dashboard was not pushed: another session was selected"));
    QVERIFY(!noticeIsError(window));
    // Another session's queue is never sent, even when it shows the same text.
    settleSelectedInspection(composer, client, {}, "kimi/docs/research", fixture.details);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
}

void TestSessionsWindow::pushTimesOut()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "Wait for the agent to finish the build"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    fixture.queue("q-other", "❯ Something else"); inspectPush(window, fixture);
    // A snapshot that arrives after the deadline never sends the queue, even one holding the message.
    window.setPushTimeoutForTests(0);
    fixture.queue("q-late", "❯ Wait for the agent to finish the build"); inspectPush(window, fixture);
    QTRY_COMPARE(notice(window), QString("The message was sent without push: the agent did not offer to send it now within 20 seconds"));
    QVERIFY(!noticeIsError(window));
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
}

void TestSessionsWindow::pushOutlastedBySlowSend()
{
    PushFixture fixture; QVERIFY(fixture.install(script(), PushFixture::Send::Slow));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "Rebuild the documentation site"); if (QTest::currentTestFailed()) return;
    // The push gives up while the send is still on its way; the message itself still goes out.
    window.setPushTimeoutForTests(0);
    QTRY_COMPARE(notice(window), QString("The message will be sent without push: it was not delivered within 20 seconds"));
    QVERIFY(!noticeIsError(window)); QCOMPARE(delivered.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, 15000);
    fixture.queue("q-slow", "❯ Rebuild the documentation site"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
}

void TestSessionsWindow::pushOfAttachmentsAloneSendsWithoutPush()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    setComposerText(composer, {}); QVERIFY(composer->addAttachment("notes.txt", "text/plain", "release notes"));
    QCOMPARE(composer->editor()->toPlainText().trimmed(), QString("[File #1]"));
    pressPush(window); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    QTRY_COMPARE(notice(window), QString("Sent without push: a message without text cannot be found in the agent's queue"));
    QVERIFY(!noticeIsError(window));
    fixture.queue("q-file", "❯ [File #1] (Read the attached file at \"/fixture/notes.txt\")"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
}

void TestSessionsWindow::pushDropsWhenSendFails_data()
{
    QTest::addColumn<bool>("uncertain"); QTest::addColumn<QString>("expected");
    QTest::newRow("refused") << false << QStringLiteral("The message was not pushed: sending did not complete");
    QTest::newRow("uncertain") << true << QStringLiteral("The message was not pushed: delivery is not confirmed");
}

void TestSessionsWindow::pushDropsWhenSendFails()
{
    QFETCH(bool, uncertain); QFETCH(QString, expected);
    PushFixture fixture; QVERIFY(fixture.install(script(), uncertain ? PushFixture::Send::Uncertain : PushFixture::Send::Refuse));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    QSignalSpy failed(window.findChild<HgsClient *>(), &HgsClient::messageFailed);
    pushMessage(window, "This message is not delivered"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(failed[0][4].toBool(), uncertain);
    QTRY_COMPARE(notice(window), expected); QVERIFY(noticeIsError(window));
    fixture.queue("q-old", "❯ This message is not delivered"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
    QTRY_COMPARE(composer->editor()->toPlainText(), QString("This message is not delivered"));
}

void TestSessionsWindow::renameChordWorksOnTheSessionsPage()
{
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/hgs/dashboard");
    int dialogs = 0;
    QTimer close; close.setInterval(10);
    connect(&close, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("renameSessionDialog");
        if (dialog && dialog->isVisible()) { ++dialogs; dialog->reject(); }
    });
    close.start();
    // Like the tab and focus commands, renaming belongs to the Sessions page.
    auto *pages = workspacePages(window); window.findChild<QPushButton *>("brandMark")->click(); QCOMPARE(pages->currentIndex(), 3);
    pressCommand(&window, "session.rename"); QTest::qWait(100);
    QCOMPARE(dialogs, 0); QCOMPARE(pages->currentIndex(), 3);
    pressCommand(&window, "go.sessions");
    pressCommand(&window, "session.rename");
    QTRY_COMPARE(dialogs, 1);
}

void TestSessionsWindow::attentionChordsStayInSearchResults()
{
    auto box = fleet().local(); const auto base = box.sessions[1]; box.sessions.clear();
    for (const auto &[name, phase] : QList<QPair<QString, QString>>{{"codex/alpha/one", "approval"}, {"claude/delta/four", "approval"},
             {"codex/beta/two", "idle"}, {"codex/gamma/three", "input"}}) {
        auto session = base; session.name = name; session.cmd = name.section('/', 0, 0); session.project = name.section('/', 1, 1);
        session.tag = name.section('/', 2); session.phase = phase; box.sessions << session;
    }
    FleetState state; state.setLocal(box, QDateTime::currentMSecsSinceEpoch());
    SessionsWindow window(program()); window.setFleet(state); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.showSession({}, "codex/gamma/three");
    // The Claude session needs attention too, but the search results do not show it.
    window.findChild<QLineEdit *>("search")->setText("codex");
    auto *results = window.findChild<QListWidget *>("searchResults"); QTRY_VERIFY(results->isVisible()); QCOMPARE(results->count(), 3);
    const auto name = [](const QListWidgetItem *item) { return item ? item->data(Qt::UserRole).toString().section('\n', 1, 1) : QString(); };
    for (int row = 0; row < results->count(); ++row) if (name(results->item(row)) == "codex/gamma/three") results->setCurrentRow(row);
    QCOMPARE(name(results->currentItem()), QString("codex/gamma/three"));
    results->setFocus(); QTRY_VERIFY(results->hasFocus());
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    pressCommand(&window, "session.nextAttention"); // Wraps to the first result.
    QCOMPARE(name(results->currentItem()), QString("codex/alpha/one")); QVERIFY(results->isVisible());
    QCOMPARE(composer->sessionKey(), QString("\ncodex/alpha/one"));
    pressCommand(&window, "session.nextAttention"); // Skips the idle session.
    QCOMPARE(name(results->currentItem()), QString("codex/gamma/three"));
    pressCommand(&window, "session.previousAttention");
    QCOMPARE(name(results->currentItem()), QString("codex/alpha/one"));
    pressCommand(&window, "session.previousAttention"); // Wraps to the last.
    QCOMPARE(name(results->currentItem()), QString("codex/gamma/three")); QVERIFY(results->isVisible());
    QCOMPARE(composer->sessionKey(), QString("\ncodex/gamma/three"));
    // Results without such a session say so.
    window.findChild<QLineEdit *>("search")->setText("beta");
    QTRY_COMPARE(results->count(), 1);
    pressCommand(&window, "session.nextAttention");
    QCOMPARE(notice(window), QString("No sessions need attention"));
}

void TestSessionsWindow::goToFollowsTheSessionList()
{
    // Projects order the session list unlike the sessions' keys.
    SessionOrganization org; const auto first = org.createGroup("First"), second = org.createGroup("Second");
    const auto empty = org.createGroup("Empty project");
    org.moveSession(org.observe("mac", "claude/infra/review", {}), first);
    org.moveSession(org.observe("arch", "codex/website/navigation", {}), first);
    org.moveSession(org.observe("arch", "codex/hgs/dashboard", {}), second);
    org.moveSession(org.observe("arch", "kimi/docs/research", {}), second);
    QSettings().setValue("workspace/organization", QJsonDocument(org.toJson()).toJson(QJsonDocument::Compact));
    QSettings().setValue("workspace/hideEmptyProjects", false);
    SessionsWindow window(program()); window.setFleet(fleet()); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *list = window.findChild<SessionList *>("sessionList");
    QStringList listed;
    for (int row = 0; row < list->count(); ++row) {
        const auto *item = list->item(row);
        if (!item->data(SessionRoles::Header).toBool() && item->data(SessionRoles::ChildId).toString().isEmpty())
            listed << item->data(SessionRoles::Key).toString();
    }
    pressCommand(&window, "go.anywhere");
    auto *goTo = window.findChild<GoToDialog *>("goToDialog"); QVERIFY(goTo); QTRY_VERIFY(goTo->isVisible());
    auto *results = goTo->findChild<QListWidget *>("goToResults"); auto *query = goTo->findChild<QLineEdit *>("goToQuery");
    QStringList others; GoToItem project;
    for (int row = 0; row < results->count(); ++row) {
        const auto item = results->item(row)->data(Qt::UserRole).value<GoToItem>();
        if (item.kind == GoToItem::Session && item.recency < 0) others << item.id;
        if (item.kind == GoToItem::Project && item.id == empty) project = item;
    }
    // Without recency, sessions follow the session list; those it does not show come last.
    QStringList expected; for (const auto &key : listed) if (others.contains(key)) expected << key;
    QVERIFY(expected.size() >= 3);
    auto sorted = expected; std::sort(sorted.begin(), sorted.end()); QVERIFY(expected != sorted);
    QCOMPARE(others.mid(0, expected.size()), expected);
    // A leading ">" lists sections and commands.
    query->setText(">"); QVERIFY(results->count() > 0);
    bool section = false;
    for (int row = 0; row < results->count(); ++row) {
        const auto kind = results->item(row)->data(Qt::UserRole).value<GoToItem>().kind;
        QVERIFY(kind == GoToItem::Section || kind == GoToItem::Command); section |= kind == GoToItem::Section;
    }
    QVERIFY(section);
    query->setText(">proj"); QCOMPARE(results->item(0)->data(Qt::UserRole).value<GoToItem>().title, QString("Projects"));
    // A project without sessions opens Projects with that project selected.
    QCOMPARE(project.title, QString("Empty project"));
    goTo->hide(); emit goTo->activated(project);
    QCOMPARE(workspacePages(window)->currentIndex(), 1);
    auto *projects = window.findChild<ProjectsDialog *>()->findChild<QListWidget *>("logicalProjects"); QVERIFY(projects->currentItem());
    QCOMPARE(projects->currentItem()->data(Qt::UserRole).toString(), empty);
    QVERIFY(projects->currentRow() > 0);
}

void TestSessionsWindow::pushBehindADialogIsNotSentAtTheDeadline()
{
    PushFixture fixture; fixture.details["cache_hint"] = QJsonObject{{"status", "cold"}}; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    // The cold cache warning holds the message: the push gives up while it is open,
    // and does not claim the message will be sent.
    const QString expected("The message was not pushed: it was not sent within 20 seconds");
    QString shown; bool error = true; int ticks = 0;
    QTimer answer; answer.setInterval(10);
    connect(&answer, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QMessageBox *>("coldCacheConfirm");
        if (!dialog || !dialog->isVisible()) return;
        window.setPushTimeoutForTests(0);
        if (notice(window) != expected && ++ticks < 1000) return;
        shown = notice(window); error = noticeIsError(window);
        answer.stop(); dialog->button(QMessageBox::Cancel)->click();
    });
    answer.start();
    pushMessage(window, "Wait behind the cold cache warning"); if (QTest::currentTestFailed()) return;
    QVERIFY(!answer.isActive());
    QCOMPARE(shown, expected); QVERIFY(!error);
    QTest::qWait(150); QCOMPARE(delivered.size(), 0); QCOMPARE(fixture.sentNow(), QStringList());
}

void TestSessionsWindow::pushReplacesAnEarlierPush()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "First message to push"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    pushMessage(window, "Second message to push"); if (QTest::currentTestFailed()) return;
    QCOMPARE(notice(window), QString("The earlier push was replaced by a new one")); QVERIFY(!noticeIsError(window));
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 2, PushTimeout);
    // Only the new push sends: a queue with the earlier message alone stays.
    fixture.queue("q-first", "❯ First message to push"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
    fixture.queue("q-both", "❯ First message to push\n❯ Second message to push"); inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-both"}, PushTimeout);
}

void TestSessionsWindow::pushWaitsForARunningSendNow()
{
    PushFixture fixture; fixture.queue("q-old", "❯ Earlier queued text");
    QVERIFY(fixture.install(script(), PushFixture::Send::Confirm, true));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    auto *client = window.findChild<HgsClient *>(); auto *composer = window.findChild<MessageComposer *>("messageComposer");
    QSignalSpy delivered(client, &HgsClient::messageSent), queued(client, &HgsClient::queueSendFinished);
    // The gated send-now request can outlast the ordinary deadline on a loaded machine.
    window.setPushTimeoutForTests(60000);
    pushMessage(window, "Push this after the earlier queue"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    // A send-now request for the earlier queue is still running when the agent queues the message.
    composer->editor()->setFocus(); QTRY_VERIFY(composer->editor()->hasFocus());
    pressCommand(&window, "queue.sendNow"); QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-old"}, PushTimeout);
    fixture.queue("q-push", "❯ Earlier queued text\n❯ Push this after the earlier queue"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList{"q-old"});
    QVERIFY2(queued.isEmpty(), "The earlier send-now request must still be running");
    QVERIFY(notice(window) != QString("Nothing queued can be sent now"));
    // Once it has finished, a later snapshot sends the queue with the message.
    QVERIFY(fixture.release());
    QTRY_COMPARE_WITH_TIMEOUT(queued.size(), 1, 15000);
    inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), (QStringList{"q-old", "q-push"}), PushTimeout);
}

void TestSessionsWindow::pushIgnoresTheCodexQueueHeader()
{
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    pushMessage(window, "interrupt"); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    // Codex's queue header says "interrupt"; only a queued item that begins with the message holds it.
    const QString header("• Messages to be submitted after next tool call (press esc to interrupt and send immediately)");
    fixture.queue("q-header", header + "\n  ↳ Check the build first"); inspectPush(window, fixture);
    fixture.queue("q-wrapped", "• Messages to be submitted after next tool call (press esc to\n  interrupt and send immediately)\n  ↳ Check the build first");
    inspectPush(window, fixture);
    fixture.queue("q-follow-up", "• Queued follow-up inputs\n  ↳ Check the build first\n    interrupt"); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
    fixture.queue("q-match", header + "\n  ↳ Check the build first\n  ↳ interrupt"); inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-match"}, PushTimeout);
}

void TestSessionsWindow::pushFindsAMessageThatStartsWithAnAttachment_data()
{
    QTest::addColumn<QString>("name"); QTest::addColumn<QString>("mime"); QTest::addColumn<QString>("reference");
    QTest::addColumn<QString>("instruction"); QTest::addColumn<QString>("path");
    QTest::newRow("file") << QStringLiteral("notes.txt") << QStringLiteral("text/plain") << QStringLiteral("[File #1]")
                          << QStringLiteral("Read the attached file at") << QStringLiteral("\"/fixture/hgs/attachments/request-one/notes.txt\"");
    // The CLI quotes the path as JSON, so Windows backslashes arrive escaped.
    QTest::newRow("image") << QStringLiteral("shot.png") << QStringLiteral("image/png") << QStringLiteral("[Image #1]")
                           << QStringLiteral("Inspect the attached image at") << QStringLiteral(R"("C:\\fixture\\hgs\\attachments\\shot.png")");
}

void TestSessionsWindow::pushFindsAMessageThatStartsWithAnAttachment()
{
    QFETCH(QString, name); QFETCH(QString, mime); QFETCH(QString, reference); QFETCH(QString, instruction); QFETCH(QString, path);
    PushFixture fixture; QVERIFY(fixture.install(script()));
    SessionsWindow window(FakeCli::program(fixture.script)); openPushSession(window, fleet(), fixture);
    if (QTest::currentTestFailed()) return;
    auto *composer = window.findChild<MessageComposer *>("messageComposer");
    QSignalSpy delivered(window.findChild<HgsClient *>(), &HgsClient::messageSent);
    // Attached first, then typed.
    setComposerText(composer, {}); QVERIFY(composer->addAttachment(name, mime, "fixture bytes"));
    composer->editor()->moveCursor(QTextCursor::End); composer->editor()->insertPlainText("Check this");
    QCOMPARE(composer->editor()->toPlainText(), reference + " Check this");
    pressPush(window); if (QTest::currentTestFailed()) return;
    QTRY_COMPARE_WITH_TIMEOUT(delivered.size(), 1, PushTimeout);
    // The CLI pastes each reference followed by the file's location (src/state/attachments.rs).
    const QString pasted = reference + " (" + instruction + ' ' + path + ") Check this";
    // Only an item that begins with the message holds it: not a header row, not
    // the middle of another item, not another message's attachment.
    fixture.queue("q-header", "• " + pasted + "\n  ↳ Rename the release branch"); inspectPush(window, fixture);
    fixture.queue("q-middle", "❯ Compare the release notes " + pasted); inspectPush(window, fixture);
    fixture.queue("q-continued", "❯ Compare the release notes\n  " + pasted); inspectPush(window, fixture);
    fixture.queue("q-other", "❯ " + QString(pasted).replace("#1]", "#2]")); inspectPush(window, fixture);
    QTest::qWait(150); QCOMPARE(fixture.sentNow(), QStringList());
    // The agent's terminal wraps the location onto the next row.
    fixture.queue("q-match", "❯ Rename the release branch\n❯ " + reference + " (" + instruction + "\n  " + path + ") Check this");
    inspectPush(window, fixture);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.sentNow(), QStringList{"q-match"}, PushTimeout);
}
