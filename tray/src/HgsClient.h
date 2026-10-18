#pragma once

#include <QHash>
#include <QByteArray>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>

class QTimer;
namespace DownloadCache { struct Pending; }

struct SessionInfo {
    QJsonObject recovery;
    QJsonObject providerError;
    QJsonObject gitStatus;
    QJsonObject goal;
    QJsonObject coordinationWait;
    QString name;      // полное имя: cmd/project[/tag]
    QString cmd;
    QString project;   // может быть null -> isNull()
    QString tag;       // может быть null -> isNull()
    int attached = 0;
    QStringList clients;   // tty приаттаченных клиентов
    qint64 created = 0;
    QString state = QStringLiteral("running"); // running | paused | stopped | archived
    QString archiveId;
    QString runId, attentionId, launchId, conversationId, replyId;
    QString accountId, accountHome;
    double replyAt = 0;
    bool unreadReply = false;
    // The reply came from a provider-scheduled turn. Jobs the agent scheduled
    // for itself, -1 when unknown or unsupported; their earliest next run.
    bool replyScheduled = false;
    int scheduledJobs = -1, recurringSchedules = -1;
    double nextScheduledAt = 0;
    int backgroundProcesses = -1;
    double backgroundStarted = 0, backgroundObservedAt = 0;
    QString backgroundCommand;
    bool reviewLater = false, attentionAcknowledged = false;
    // Shared read state from the session's host. sharedRead is false for hosts
    // whose hgs predates `hgs read`; those keep this device's own marks.
    bool sharedRead = false;
    QString attentionSignature, readReplyId, readAttentionSignature;
    double archivedAt = 0;
    bool resumable = false;
    bool terminalSupported = true, terminalReported = false, bulkLifecycleSupported = true, actionsReported = false;
    QString terminalReason;
    QSet<QString> allowedActions;
    QJsonObject actionReasons;
    QString activity;
    bool tracked = false;
    QString phase, model, effort, prompt, currentTool, toolDetail;
    QString cwd, canonicalCwd, gitCommonDir, cwdSource, gitRoot, gitBranch, gitWorktreeName, gitMetadataState;
    bool gitWorktree = false, gitDetached = false;
    QString activitySummary, activityDetail, subagentSource;
    bool subagentCountsComplete = false;
    int subagentActiveCount = 0, subagentCompletedCount = 0, subagentTotalCount = -1;
    QJsonArray subagentPreviews;
    QJsonObject subagents;
    QString processState, conversationState, runtimeState;
    double lastEventAt = 0, turnStarted = 0, compactionStarted = 0, providerStatusAt = 0;
    int subagentCount = 0;
    // Pending optional Codex questions ("optional_questions"): the exact count,
    // the IDs the host lists (the newest ones) and the newest question's time.
    // -1 means unknown: null while the host's question index is incomplete, or
    // a malformed value. optionalQuestionsReported is false for hosts whose hgs
    // predates the field; they never show a count. These never mean attention.
    int optionalQuestions = -1;
    QStringList optionalQuestionIds;
    double optionalQuestionsLatestAt = 0;
    bool optionalQuestionsReported = false;
    bool replyInProgress() const {
        return state == QLatin1String("running") && processState != QLatin1String("exited")
            && activity != QLatin1String("unknown") && !(conversationState == QLatin1String("ended") && activity != QLatin1String("idle"))
            && (activity == QLatin1String("busy") || backgroundProcesses > 0 || subagentActiveCount > 0
                || (activity == QLatin1String("idle") && phase == QLatin1String("idle") && hasCoordinationWait()));
    }
    bool hasCoordinationWait() const {
        return coordinationWait.value("schema").toInt() == 1 && coordinationWait.value("status") == "ok"
            && !runId.isEmpty() && !conversationId.isEmpty() && coordinationWait.value("run_id") == runId
            && coordinationWait.value("conversation_id") == conversationId && coordinationWait.value("count").toInt() > 0
            && coordinationWait.value("count").toInt() <= 1024
            && coordinationWait.value("count").toDouble() == coordinationWait.value("count").toInt();
    }
    // Intermediate replies remain unread; only their notification presentation waits.
    bool visibleUnreadReply(bool online = true) const {
        return state != QLatin1String("archived") && unreadReply && (!online || !replyInProgress());
    }
    bool needsAttention() const { return (needsAction() && !attentionAcknowledged)
        || (state != QLatin1String("archived") && (visibleUnreadReply() || reviewLater)); }
    bool needsAction() const {
        const auto recoveryState = recovery.value("state").toString();
        const bool recovering = recoveryState == "waiting" || recoveryState == "dispatching" || recoveryState == "retrying";
        return state == QLatin1String("running") && processState != QLatin1String("exited")
            && (phase == QLatin1String("approval") || phase == QLatin1String("input")
                || (phase == QLatin1String("error") && (!recovering || providerError.value("error_kind")=="quota"
                    || providerError.value("error_kind")=="provider_policy"))
                || recoveryState == "uncertain" || recoveryState == "blocked" || recoveryState == "exhausted");
    }
};

struct BoxState {
    bool archivePaging = false;
    QJsonObject metrics;
    // A peer's box is keyed by the alias it was polled under (a name in this
    // computer's "peers"), whatever that machine calls itself.
    QString host;
    // The "host" a peer's answer reported: the name that machine gives itself
    // (HGS_SELF or its host name), which may differ from the alias. For
    // diagnostics only; empty except on peer answers.
    QString reportedHost;
    bool ok = false;
    QString error;                     // "offline" | "bad_response" | пусто
    QHash<QString, QString> projects;  // имя -> каталог
    QList<SessionInfo> sessions;
    QStringList peers;
    bool peersKnown = false;
    // False for a control-only computer (hgs.exe on Windows): it manages remote
    // machines but runs no sessions, accounts or recovery of its own. hgs omits
    // "local_sessions" unless it is false.
    bool localSessions = true;
    // Windows: the session host as this box's CLI found it ("local_host": state,
    // pid, protocol, conpty, integrity, windows_session, and error when it is
    // unavailable). Empty on Unix and from an older CLI.
    QJsonObject localHost;
    // "ended_launches" by launch ID: launches whose session ended before this
    // listing without a listed session or record ("name", "code" or null,
    // "output", its last lines). Empty from an older CLI.
    QHash<QString, QJsonObject> endedLaunches;
};

struct ProjectInfo {
    QString name;
    QString dir;
    bool fromAnsible = false;   // src == "ansible": править нельзя, только переопределить
    bool exists = false;        // каталог существует на момент опроса
};
// Не обязательно для доставки projectsReady() через QueuedConnection -- проверено
// отдельным пробником на Qt 6.11.2: новый connect-синтаксис сам выводит QMetaType
// для обычных value-типов и без макроса, сигнал доходит с корректными данными.
// Регистрируем явно всё равно: это не про доставку сигнала, а на будущее -- редактору
// из следующей задачи наверняка понадобится класть ProjectInfo в QVariant (item/model
// data), а там без Q_DECLARE_METATYPE поведение уже зависит от версии Qt.
Q_DECLARE_METATYPE(ProjectInfo)

struct MessageAttachment {
    QString name;
    QString mime;
    QByteArray data;
    QString reference;
};

// Единственное, что знает про CLI. Не знает ни про tmux, ни про ssh: и то и другое —
// знание CLI, дублировать его здесь запрещено.
class HgsClient : public QObject {
    Q_OBJECT
public:
    explicit HgsClient(QString hgsPath, QObject *parent = nullptr);
    ~HgsClient() override;
    QString executable() const { return m_hgs; }
    // Read the configured executable, including older CLIs without snapshot metadata.
    void requestVersion();
    void requestSessionReference(const QString &host, const QString &name, const QString &run,
                                 const QString &conversation, const QString &archive = {}, const QString &agent = {});
    quint64 requestRecovery(const QString &host, const QString &operation, const QJsonObject &payload = {});

    // Асинхронно: `hgs ls --json --local`. Один exec, единицы миллисекунд.
    void requestLocal();
    void launchDetachedSession(const QString &host, const QString &agent, const QString &target, const QString &name, const QString &account, const QString &launchId, const QString &model = {});
    void launchNativeSession(const QString &host, const QString &target, const QString &name, const QString &account, const QString &launchId);
    // Асинхронно: `hgs @<peer> ls --json`. Пир, который спит, — это ok:false, а не ошибка.
    // peerReady() carries a box whose host is this alias (see BoxState::host).
    void requestPeer(const QString &peer);
    // State reads in flight: "" for this computer, otherwise the polled aliases.
    QSet<QString> pollingHosts() const;
    // Project lists and directory browsing are read on the selected machine.
    // Directory request IDs let views discard replies to superseded navigation.
    void requestProjects(const QString &host = {});
    quint64 requestAccounts(const QString &host, const QStringList &arguments = {QStringLiteral("ls")}, const QByteArray &input = {});
    quint64 requestMachines(const QStringList &arguments = {QStringLiteral("ls")});
    // hgs [@host] mobile …: phones of the workspace that host's mobile connection
    // reaches. Probes the host CLI first, so an older one is never asked.
    quint64 requestMobile(const QString &host, const QStringList &arguments);
    quint64 requestSwarm(const QStringList &arguments = {QStringLiteral("get")}, const QJsonObject &input = {});
    // `hgs session-host <action> --json` on this computer (Windows, spec D2):
    // install enables local sessions and stages the stable CLI and a host image.
    quint64 requestSessionHost(const QString &action);
    // `hgs presence set --json`. At most one write runs; while it does, only the
    // newest sample waits, so a slow CLI never builds a queue of stale samples.
    void setPresence(double idleSeconds, bool locked);
    void setupMachine(const QString &alias, const QString &source = {});
    QJsonObject worktreeSnapshot(const QString &host, const QString &path) const;
    QJsonObject gitStatusSnapshot(const QString &host, const QString &path) const;
    void requestGitStatus(const QString &host, const QString &path);
    quint64 requestWorktrees(const QString &host, const QString &path, bool refresh = false);
    quint64 createWorktree(const QString &host, const QString &path, const QString &commonDir,
                           const QString &branch, const QString &base, const QString &destination);
    // Cleanup verdicts are computed on the owning machine. Whole-repository reviews
    // stay cached for five minutes for the Clean up reminder; removal repeats every
    // check there. Discard needs its own content proof; failures are never replayed.
    quint64 reviewWorktrees(const QString &host, const QString &path, const QString &worktree = {});
    quint64 inspectWorktree(const QString &host, const QString &path, const QString &commonDir, const QString &file = {}, int offset = 0, bool refresh = false);
    // Browsing may retain a review for an hour; cleanup callers keep the default five-minute limit.
    void invalidateWorktreeInspection(const QString &host, const QString &path, const QString &commonDir);
    void invalidateWorktreeCache(const QString &host, const QString &commonDir);
    QJsonObject worktreeReview(const QString &host, const QString &commonDir, int maxAgeSeconds = 300) const;
    quint64 removeWorktree(const QString &host, const QString &path, const QString &commonDir,
                           const QString &fingerprint, bool deleteBranch, const QString &ignoredFingerprint = {}, const QString &discardFingerprint = {});
    quint64 requestDirectories(const QString &host, const QString &path, bool hidden = false);
    quint64 createDirectory(const QString &host, const QString &path);
    // Whether an inspection of this session is running; one at a time per session.
    bool inspectionInFlight(const QString &host, const QString &name, const QString &archiveId = QString()) const
    { return m_inspectionsInFlight.contains(host + QLatin1Char('\n') + name + QLatin1Char('\n') + archiveId); }
    bool requestInspection(const QString &host, const QString &name, qint64 after = 0, const QString &archiveId = {}, bool includeProcesses = true);
    quint64 requestProcess(const QString &host, const QString &name, const QString &processId, const QString &run,
                           const QString &conversation, const QString &archive, const QString &generation, bool stop = false);
    void requestSubagentInspection(const QString &host, const QString &name, const QString &agentId, const QString &archiveId = {});
    quint64 requestEffort(const QString &host, const QString &name, const QString &effort, const QString &run, const QString &conversation);
    quint64 requestSettings(const QString &host, const QString &name, const QString &model, const QString &effort,
                            const QString &run, const QString &conversation, const QString &expectedPendingId = {});
    quint64 requestSearch(const QString &host, const QString &query);
    quint64 requestAttachment(const QString &host, const QString &name, const QJsonObject &file,
                              const QString &conversation, const QString &archive = {}, const QString &agent = {});
    quint64 downloadFile(const QString &host, const QString &path, const QString &destination);
    quint64 downloadFileToCache(const QString &host, const QString &path);
    quint64 requestApprovalLog(const QString &operation, const QJsonObject &data = {});
    static constexpr qsizetype MaximumMessageBytes = 64 * 1024;
    static constexpr qsizetype MaximumAttachmentBytes = 10 * 1024 * 1024;
    static constexpr qsizetype MaximumAttachmentsBytes = 20 * 1024 * 1024;
    static constexpr qsizetype MaximumAttachments = 8;
    quint64 requestSendMessage(const QString &host, const QString &name, const QString &text,
                               const QList<MessageAttachment> &attachments,
                               const QString &expectedRunId, const QString &expectedConversationId = {}, const QString &agentId = {}, const QString &compactionId = {});
    quint64 requestQueueSendNow(const QString &host, const QString &name, const QString &run, const QString &conversation, const QString &queue);
    // An untracked session (a shell from `hgs sh -d`) has no run identity: pass
    // empty run and conversation and its listed creation time, which binds the
    // copy to that live session instead.
    quint64 requestStageFiles(const QString &host, const QString &name, const QStringList &paths,
                             const QString &run, const QString &conversation, qint64 untrackedCreated = -1);
    quint64 requestCompactContext(const QString &host, const QString &name, const QString &run, const QString &conversation);
    quint64 requestClearContext(const QString &host, const QString &name, const QString &run, const QString &conversation, bool dismissPrompt = false);
    quint64 requestInterrupt(const QString &host, const QString &name, const QString &run,
                             const QString &conversation, double turnStarted);
    quint64 requestAnswerQuestion(const QString &host, const QString &name,
                                  const QJsonObject &question, const QJsonArray &answers, const QString &durableRequestId = {});
    // `hgs [@host] read <session> --json`: the host applies each field only while
    // it is current. A JSON answer, including not_found, is delivered.
    quint64 requestRead(const QString &host, const QString &name, const QJsonObject &payload);

    // Чистые функции разбора: тестируются без запуска процессов.
    void cancelArchivePages();
    quint64 requestArchivePage(const QString &host, const QJsonObject &query);
    static QList<SessionInfo> parseSessions(const QJsonArray &rows);
    static BoxState parseBox(const QByteArray &json, QString *error);
    static QList<BoxState> parseFleet(const QByteArray &json, QString *error);
    static QList<ProjectInfo> parseProjects(const QByteArray &json, QString *error);

    // Пишущие операции. Все возвращают исход одним сигналом writeDone: вызывающему
    // нужно ровно «получилось или нет и что сказал hgs», а не четыре разных ответа.
    void forkSession(const QString &host, const QString &name, const QString &tag, const QString &archive, const QString &run, const QString &conversation);
    void terminateSession(const QString &host, const QString &name, const QString &run);
    void killSession(const QString &host, const QString &name);   // host пуст = свой бокс
    void pauseSession(const QString &host, const QString &name);
    void openNativeUi(const QString &host, const QString &name);
    quint64 requestNativeUi(const QString &host, const QString &name);
    void resumeSession(const QString &host, const QString &name);
    // A new conversation from the stopped run's own launch settings.
    void startFreshSession(const QString &host, const QString &name, const QString &run);
    void archiveSession(const QString &host, const QString &name);
    void renameSession(const QString &host, const QString &name, const QString &newName, const QString &archiveId = {});
    void restoreArchive(const QString &host, const QString &name, const QString &archiveId);
    void forgetArchive(const QString &host, const QString &name, const QString &archiveId);
    void pauseAll(const QString &host, int count);
    void resumeAll(const QString &host, int count);
    void projectSet(const QString &name, const QString &dir, const QString &host = {});
    void projectRemove(const QString &name, const QString &host = {});

signals:
    void archivePageReady(quint64 request, const QString &host, const QJsonObject &data, const QString &error);
    void versionReady(const QString &version); // Empty when unavailable or unrecognized.
    void sessionReferenceReady(const QString &uri);
    void sessionReferenceFailed(const QString &reason);
    void sessionActionFinished(quint64 request, bool ok, const QJsonObject &result, const QString &error);
    void recoveryFinished(quint64 request, bool ok, const QJsonObject &result, const QString &error);
    void attachmentReady(quint64 request, const QJsonObject &file, const QByteArray &bytes);
    void fileDownloaded(quint64 request, const QString &destination);
    void fileDownloadFailed(quint64 request, const QString &error);
    void downloadCacheWarning(const QString &error);
    void approvalLogFinished(quint64 request, const QString &operation, const QJsonObject &result, const QString &error);
    void attachmentFailed(quint64 request, const QString &detail);
    void nativeUiReady(quint64 request, const QJsonObject &connection);
    void nativeUiFailed(quint64 request, const QString &error);
    void nativeSessionLaunched(const QString &launchId, bool ok, const QString &error);
    void settingsFinished(quint64 request, bool ok, const QJsonObject &receipt, const QString &error);
    void effortFinished(quint64 request, bool ok, const QJsonObject &receipt, const QString &error);
    void searchReady(quint64 request, const QString &host, const QJsonObject &data);
    void searchFailed(quint64 request, const QString &host, const QString &error);
    void accountsReady(quint64 request, const QString &host, const QJsonObject &result);
    void accountsFailed(quint64 request, const QString &host, const QString &error);
    void mobileReady(quint64 request, const QString &host, const QJsonObject &result);
    void mobileFailed(quint64 request, const QString &host, const QString &error);
    void machinesReady(quint64 request, const QJsonObject &result);
    void machinesFailed(quint64 request, const QString &error);
    void swarmReady(quint64 request, const QJsonObject &result);
    void swarmFailed(quint64 request, const QString &error);
    void sessionHostReady(quint64 request, const QJsonObject &result);
    void sessionHostFailed(quint64 request, const QString &error);
    void presenceFinished(const QString &error); // Empty on success.
    void machineSetupOutput(const QString &text);
    void machineSetupFinished(bool ok, const QString &error);
    void questionAnswered(quint64 request, const QString &host, const QString &name, const QJsonObject &receipt);
    void questionAnswerSubmitted(quint64 request, const QString &host, const QString &name, const QJsonObject &receipt);
    void questionAnswerFailed(quint64 request, const QString &host, const QString &name,
                              const QString &detail, bool deliveryUncertain);
    void readFinished(quint64 request, bool delivered, const QJsonObject &result, const QString &error);
    void queueSendFinished(quint64 request, bool ok, const QJsonObject &receipt, const QString &error);
    void messageSent(quint64 request, const QString &host, const QString &name, const QJsonObject &receipt);
    void messageFailed(quint64 request, const QString &host, const QString &name, const QString &detail, bool deliveryUncertain);
    void sessionWriteDone(const QString &host);
    void stateReadFailed(const QString &host, const QString &detail);
    void stateReadStarted(const QString &host);
    void inspectionLoadingChanged(const QString &host, const QString &name, const QString &archiveId, const QString &agentId, bool loading);
    void inspectionReady(const QString &host, const QString &name, const QJsonObject &data, const QString &archiveId = QString());
    void processFinished(quint64 request, const QJsonObject &data, const QString &error);
    void inspectionFailed(const QString &host, const QString &name, const QString &detail, const QString &archiveId = QString());
    void subagentInspectionReady(const QString &host, const QString &name, const QString &agentId, const QString &archiveId, const QJsonObject &data);
    void subagentInspectionFailed(const QString &host, const QString &name, const QString &agentId, const QString &archiveId, const QString &error);
    void localReady(const BoxState &box);
    void peerReady(const BoxState &box);
    void projectsReady(const QList<ProjectInfo> &projects);
    void projectsForHostReady(const QString &host, const QList<ProjectInfo> &projects);
    void projectsFailed(const QString &host, const QString &detail);
    void worktreeCacheInvalidated(const QString &host, const QString &commonDir);
    void worktreesReady(quint64 request, const QString &host, const QString &path, const QJsonObject &data);
    void worktreesFailed(quint64 request, const QString &host, const QString &path, const QString &detail);
    void gitStatusChanged(const QString &host, const QString &path);
    void worktreeCreated(quint64 request, bool ok, const QJsonObject &result, const QString &error);
    void worktreeReviewReady(quint64 request, const QString &host, const QString &path, const QJsonObject &data);
    void worktreeReviewFailed(quint64 request, const QString &host, const QString &path, const QString &detail);
    void worktreeInspectionFinished(quint64 request, const QString &host, const QString &path, const QString &file, const QJsonObject &data, const QString &error);
    void worktreeRemoved(quint64 request, bool ok, const QJsonObject &result, const QString &error);
    void worktreeCleanupFinished(const QString &host, const QString &operation, const QJsonObject &result);
    void directoriesReady(quint64 request, const QString &host, const QJsonObject &data);
    void directoryCreated(quint64 request, const QString &host, const QJsonObject &data);
    void directoryCreationFailed(quint64 request, const QString &host, const QString &detail);
    void directoriesFailed(quint64 request, const QString &host, const QString &detail);
    // Процесс не запустился или не уложился в таймаут. Это НЕ то же самое, что ok:false:
    // здесь сломан сам вызов hgs, и трей должен показать состояние «?».
    void failed(const QString &what, const QString &detail);
    // op — человеческое имя операции для сообщения об ошибке ("kill claude/x",
    // "save project scratch"); detail — stderr от hgs, уже обрезанный.
    void writeDone(const QString &op, bool ok, const QString &detail);

private:
    quint64 m_archiveRequest = 0;
    QHash<QString,QObject *> m_archiveJobs;
    void transferFile(quint64 id, const QString &host, const QString &path, const QString &destination,
                      const std::shared_ptr<DownloadCache::Pending> &pending = {});
    void scheduleDownloadCacheTrim();
    void trimDownloadCache();
    QTimer *m_downloadCacheTimer = nullptr;
    bool m_downloadCacheTrimBusy = false;
    void pumpGitStatus();
    QHash<QString, QJsonObject> m_gitStatusCache;
    QHash<QString, qint64> m_gitStatusRequestedAt;
    QList<QPair<QString, QString>> m_gitStatusQueue;
    QSet<QString> m_gitStatusInFlight;
    quint64 runSessionAction(const QString &host, const QString &name, const QString &operation, QJsonObject payload);
    // Общий каркас пишущего вызова: успех = выход 0, stderr отдаём как есть -- hgs
    // сам формулирует внятные ошибки ("'x' comes from Ansible ... shadow it with
    // 'hgs project set'"), переформулировать их здесь значит потерять смысл.
    // timeoutMs -- в отличие от requestLocal()/requestProjects() тут нет единой
    // константы: локальный kill и правка карты -- это exec без сети, а kill пира идёт
    // через ssh и может повиснуть на спящем ноуте (см. requestPeer) -- вызывающий сам
    // выбирает бюджет под конкретную команду.
    void runWrite(const QString &op, const QStringList &args, int timeoutMs,
                  bool sessionWrite = false, const QString &host = QString());

    QString m_hgs;
    // Алиасы пиров, чей "@peer ls --json" сейчас в полёте (см. requestPeer в .cpp).
    // Спящий ноут не отвечает все 15с таймаута -- без этой метки каждый тик меню/фона
    // добавлял бы ещё один параллельный ssh-процесс поверх уже висящих.
    QSet<QString> m_peerInFlight;
    bool m_localInFlight = false;
    bool m_versionInFlight = false;
    QSet<QString> m_inspectionsInFlight;
    quint64 m_directoryRequest = 0, m_worktreeRequest = 0;
    QHash<QString, QList<QPair<quint64, QString>>> m_worktreesInFlight;
    QHash<QString, QList<quint64>> m_reviewsInFlight;
    QHash<QString, QJsonObject> m_worktreeReviews;
    QHash<QString, QJsonObject> m_worktreeInspections;
    QHash<QString, QList<quint64>> m_worktreeInspectionsInFlight;
    QHash<QString, quint64> m_worktreeCacheGenerations;
    quint64 m_nativeUiRequest = 0;
    quint64 m_searchRequest = 0;
    quint64 m_attachmentRequest = 0;
    quint64 m_fileDownloadRequest = 0;
    quint64 m_approvalLogRequest = 0;
    bool m_approvalLogSupported = false;
    quint64 m_effortRequest = 0;
    quint64 m_settingsRequest = 0;
    quint64 m_recoveryRequest = 0;
    quint64 m_machineRequest = 0;
    quint64 m_mobileRequest = 0;
    quint64 m_swarmRequest = 0;
    quint64 m_sessionHostRequest = 0;
    quint64 m_accountRequest = 0;
    bool m_machineSetupInFlight = false;
    quint64 m_messageRequest = 0;
    quint64 m_sessionActionRequest = 0;
    QSet<QString> m_messagesInFlight;
    quint64 m_answerRequest = 0;
    quint64 m_readRequest = 0;
    bool m_presenceInFlight = false, m_presencePending = false;
    QPair<double, bool> m_pendingPresence;
    QSet<QString> m_answersInFlight;
};
