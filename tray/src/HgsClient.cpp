#include "HgsClient.h"
#include "ProcessRunner.h"
#include "LocalFile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QFileInfo>
#include <QFile>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QRegularExpression>
#include <QSettings>
#include <QDateTime>
#include <QCryptographicHash>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

namespace {

// The executable a start would use: a path as given, otherwise a PATH lookup
// (which honors PATHEXT on Windows, where C:\Zerus\hgs.exe is a path as well).
QString resolvedProgram(const QString &program)
{
    bool path = program.contains(QLatin1Char('/'));
#ifdef Q_OS_WIN
    path = path || program.contains(QLatin1Char('\\')) || QFileInfo(program).isAbsolute();
#endif
    return path ? program : QStandardPaths::findExecutable(program);
}

ProcessRunner::Request processRequest(const QString &program, const QStringList &arguments)
{
    ProcessRunner::Request request;
#ifdef Q_OS_MACOS
    // Qt encodes argv with QFile::encodeName(), which decomposes Unicode on
    // macOS. Session names are byte identities, not filesystem names: changing
    // NFC to NFD can target another session or make an existing one disappear.
    // Environment values use UTF-8 without normalization. The ASCII-only sh
    // trampoline expands those values as quoted data, removes its private
    // variables, and execs the CLI with the original argv and process identity.
    auto environment = QProcessEnvironment::systemEnvironment();
    QStringList values{program}; values.append(arguments);
    QStringList variables;
    QString script = QStringLiteral("set --");
    for (qsizetype i = 0; i < values.size(); ++i) {
        const QString variable = QStringLiteral("_HGS_ZERUS_ARG_%1").arg(i);
        variables.append(variable); environment.insert(variable, values[i]);
        script += QStringLiteral(" \"$%1\"").arg(variable);
    }
    script += QStringLiteral("; unset %1; exec \"$@\"").arg(variables.join(' '));
    request.program = QStringLiteral("/bin/sh");
    request.arguments = {QStringLiteral("-c"), script};
    request.environment = environment;
#else
    request.program = program;
    request.arguments = arguments;
#endif
    return request;
}

void configureProcess(QProcess *process, const QString &program, const QStringList &arguments)
{
    const auto request = processRequest(program, arguments);
    process->setProgram(request.program); process->setArguments(request.arguments);
    if (request.environment) process->setProcessEnvironment(*request.environment);
}

// null -> QString(), не пустая строка "": вызывающий код различает их через isNull().
QString nullableString(const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined())
        return QString();
    return v.toString();
}

SessionInfo sessionFromJson(const QJsonObject &obj)
{
    SessionInfo s;
    s.recovery = obj.value("recovery").toObject();
    s.name = obj.value(QStringLiteral("name")).toString();
    s.cmd = obj.value(QStringLiteral("cmd")).toString();
    s.project = nullableString(obj.value(QStringLiteral("project")));
    s.tag = nullableString(obj.value(QStringLiteral("tag")));
    s.attached = obj.value(QStringLiteral("attached")).toInt();
    s.created = static_cast<qint64>(obj.value(QStringLiteral("created")).toDouble());
    s.conversationId = obj.value("conversation_id").toString();
    s.replyId = obj.value("reply_id").toString(); s.replyAt = obj.value("reply_at").toDouble();
    s.replyScheduled = obj.value("reply_scheduled").toBool();
    const auto schedules = obj.value("schedules").toObject();
    s.scheduledJobs = schedules.value("count").toInt(-1);
    s.recurringSchedules = schedules.value("recurring").toInt(-1);
    s.nextScheduledAt = schedules.value("next_at").toDouble();
    const auto work = obj.value("process_work").toObject();
    s.backgroundProcesses = work.value("count").toInt(-1);
    s.backgroundStarted = work.value("started_at").toDouble();
    s.backgroundObservedAt = work.value("sampled_at").toDouble();
    s.backgroundCommand = work.value("command").toString();
    s.runId = obj.value(QStringLiteral("run_id")).toString();
    s.launchId = obj.value(QStringLiteral("launch_id")).toString();
    s.attentionId = obj.value(QStringLiteral("attention_id")).toString();
    s.sharedRead = obj.contains(QStringLiteral("attention_signature"));
    s.attentionSignature = obj.value(QStringLiteral("attention_signature")).toString();
    const auto read = obj.value(QStringLiteral("read")).toObject();
    if (!s.conversationId.isEmpty() && read.value("conversation_id").toString() == s.conversationId) {
        s.readReplyId = read.value("reply_id").toString();
        s.readAttentionSignature = read.value("attention_signature").toString();
    }
    s.state = obj.value(QStringLiteral("state")).toString(QStringLiteral("running"));
    s.archiveId = s.state == QLatin1String("archived") ? obj.value(QStringLiteral("archive_id")).toString() : QString();
    s.archivedAt = obj.value(QStringLiteral("archived_at")).toDouble();
    s.resumable = obj.value(QStringLiteral("resumable")).toBool();
    s.terminalReported = obj.value("terminal_supported").isBool();
    s.terminalSupported = obj.value("terminal_supported").toBool(s.cmd != "dsh" && s.cmd != "opencode");
    s.bulkLifecycleSupported = obj.value("bulk_lifecycle_supported").toBool(s.cmd != "opencode");
    s.terminalReason = obj.value("terminal_reason").toString();
    s.actionsReported = obj.value("allowed_actions").isArray();
    for (const auto &action : obj.value("allowed_actions").toArray()) if (action.isString()) s.allowedActions.insert(action.toString());
    s.actionReasons = obj.value("action_reasons").toObject();
    s.activity = obj.value(QStringLiteral("activity")).toString();
    s.tracked = obj.value(QStringLiteral("tracked")).toBool();
    s.accountId = obj.value("account_id").toString(); s.accountHome = obj.value("account_home").toString();
    s.phase = obj.value(QStringLiteral("phase")).toString();
    s.processState = obj.value(QStringLiteral("process_state")).toString();
    s.conversationState = obj.value(QStringLiteral("conversation_state")).toString();
    s.runtimeState = obj.value(QStringLiteral("runtime_state")).toString();
    s.model = obj.value(QStringLiteral("model")).toString();
    s.goal = obj.value(QStringLiteral("goal")).toObject();
    s.effort = obj.value(QStringLiteral("effort")).toString();
    s.prompt = obj.value(QStringLiteral("prompt")).toString();
    s.currentTool = obj.value(QStringLiteral("current_tool")).toString();
    s.toolDetail = obj.value(QStringLiteral("tool_detail")).toString();
    s.lastEventAt = obj.value(QStringLiteral("last_event_at")).toDouble();
    s.turnStarted = obj.value(QStringLiteral("turn_started")).toDouble();
    s.compactionStarted = obj.value(QStringLiteral("compaction_started")).toDouble();
    s.subagentCount = obj.value(QStringLiteral("subagent_count")).toInt();
    s.cwd = obj.value("cwd").toString(); s.cwdSource = obj.value("cwd_source").toString();
    s.canonicalCwd=obj.value("cwd_canonical").toString();s.gitCommonDir=obj.value("git_common_dir").toString();
    s.gitRoot = obj.value("git_root").toString(); s.gitBranch = obj.value("git_branch").toString();
    s.gitWorktree = obj.value("git_worktree").toBool(); s.gitDetached = obj.value("git_detached").toBool();
    s.gitWorktreeName = obj.value("git_worktree_name").toString(); s.gitMetadataState = obj.value("git_metadata_state").toString();
    s.activitySummary = obj.value("activity_summary").toString(); s.activityDetail = obj.value("activity_detail").toString();
    s.providerError = obj.value("provider_error").toObject();
    s.providerStatusAt = obj.value("provider_status_at").toDouble();
    s.subagentSource = obj.value("subagent_source").toString();
    s.subagentCountsComplete = obj.value("subagent_counts_complete").toBool();
    s.subagentActiveCount = obj.value("subagent_active_count").toInt(s.subagentCount);
    s.subagentCompletedCount = obj.value("subagent_completed_count").toInt();
    s.subagentTotalCount = obj.value("subagent_total_count").toInt(-1);
    s.subagentPreviews = obj.value("subagent_previews").toArray();
    s.subagents = obj.value("subagents").toObject();
    const QJsonArray clientsArr = obj.value(QStringLiteral("clients")).toArray();
    s.clients.reserve(clientsArr.size());
    for (const QJsonValue &c : clientsArr)
        s.clients << c.toString();
    return s;
}

// Общий разбор одного JSON-объекта "бокса" (используется и parseBox, и parseFleet —
// не дублировать поля).
BoxState boxFromObject(const QJsonObject &obj)
{
    BoxState box;
    box.metrics = obj.value("metrics").toObject();
    box.host = obj.value(QStringLiteral("host")).toString();
    box.ok = obj.value(QStringLiteral("ok")).toBool();
    box.error = obj.value(QStringLiteral("error")).toString();
    const auto errorDetail = obj.value(QStringLiteral("error_detail")).toString();
    if (!errorDetail.isEmpty()) box.error = errorDetail;
    box.localSessions = obj.value(QStringLiteral("local_sessions")).toBool(true);
    box.peersKnown = obj.value("peers").isArray();
    for (const auto &peer : obj.value("peers").toArray()) if (peer.isString()) box.peers.append(peer.toString());

    const QJsonObject projectsObj = obj.value(QStringLiteral("projects")).toObject();
    for (auto it = projectsObj.constBegin(); it != projectsObj.constEnd(); ++it)
        box.projects.insert(it.key(), it.value().toString());

    const QJsonArray sessionsArr = obj.value(QStringLiteral("sessions")).toArray();
    box.sessions.reserve(sessionsArr.size());
    for (const QJsonValue &sv : sessionsArr)
        box.sessions << sessionFromJson(sv.toObject());

    return box;
}

// Разбор одного элемента `hgs project ls --json`. src в самом JSON -- строка
// ("ansible"|"local"), не булево: ансибл-происхождение читаем сравнением, а не
// доверяем CLI прислать готовый bool -- формат остаётся человекочитаемым снаружи
// (то же самое видно и в `hgs project ls --json | python3 -m json.tool`).
ProjectInfo projectFromJson(const QJsonObject &obj)
{
    ProjectInfo p;
    p.name = obj.value(QStringLiteral("name")).toString();
    p.dir = obj.value(QStringLiteral("dir")).toString();
    p.fromAnsible = obj.value(QStringLiteral("src")).toString() == QLatin1String("ansible");
    p.exists = obj.value(QStringLiteral("exists")).toBool();
    return p;
}

} // namespace

HgsClient::HgsClient(QString hgsPath, QObject *parent)
    : QObject(parent)
    // Пустой путь значит "искать в PATH" — так и QProcess::start понимает голое имя без '/'.
    , m_hgs(hgsPath.isEmpty() ? QStringLiteral("hgs") : std::move(hgsPath))
{
}

quint64 HgsClient::requestRecovery(const QString &host, const QString &operation, const QJsonObject &data)
{
    const auto request = ++m_recoveryRequest;
    QStringList args{"recovery", operation}; if (!host.isEmpty()) args.prepend('@' + host);
    auto process = processRequest(m_hgs, args); process.timeoutMs = host.isEmpty() ? 10000 : 25000;
    process.input = operation != "get" ? QJsonDocument(data).toJson(QJsonDocument::Compact) : QByteArray();
    ProcessRunner::run(std::move(process), this, [this, request, host, operation](const ProcessRunner::Result &result) {
        const auto fail = [this, request](const QString &error) { emit recoveryFinished(request, false, {}, error); };
        if (result.outcome == ProcessRunner::Result::TimedOut) return fail(tr("Recovery request timed out. Reload its state before trying again."));
        if (result.outcome == ProcessRunner::Result::FailedToStart) return fail(result.errorString);
        if (result.exitCode || result.exitStatus != QProcess::NormalExit) {
            const auto error = QString::fromUtf8(result.standardError).trimmed().left(2000);
            return fail(error.isEmpty() ? tr("Could not update recovery. Reload its state.") : error);
        }
        QJsonParseError error; const auto doc = QJsonDocument::fromJson(result.standardOutput, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) return fail(tr("Invalid recovery response"));
        emit recoveryFinished(request, true, doc.object(), {}); if (operation != "get") emit sessionWriteDone(host);
    });
    return request;
}

HgsClient::~HgsClient()
{
    // Worker launches (ProcessRunner) are killed when this object is destroyed.
    // Direct QProcess children remain for interactive transfers: QProcess's
    // destructor waits and can emit finished(). Disconnect while the callback's
    // captured maps and owner are still alive, before QObject deletes children.
    for (auto *proc : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) {
        for (auto *timer : proc->findChildren<QTimer *>()) timer->stop();
        proc->disconnect(); proc->blockSignals(true);
        if (proc->state() != QProcess::NotRunning) { proc->kill(); proc->waitForFinished(1000); }
    }
}

BoxState HgsClient::parseBox(const QByteArray &json, QString *error)
{
    if (error)
        error->clear();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) {
            *error = perr.error != QJsonParseError::NoError
                ? perr.errorString()
                : QStringLiteral("expected a JSON object");
        }
        return BoxState{};
    }

    return boxFromObject(doc.object());
}

QList<BoxState> HgsClient::parseFleet(const QByteArray &json, QString *error)
{
    if (error)
        error->clear();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error) {
            *error = perr.error != QJsonParseError::NoError
                ? perr.errorString()
                : QStringLiteral("expected a JSON array");
        }
        return {};
    }

    QList<BoxState> boxes;
    const QJsonArray arr = doc.array();
    boxes.reserve(arr.size());
    for (const QJsonValue &v : arr)
        boxes << boxFromObject(v.toObject());
    return boxes;
}

QList<ProjectInfo> HgsClient::parseProjects(const QByteArray &json, QString *error)
{
    if (error)
        error->clear();

    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error) {
            *error = perr.error != QJsonParseError::NoError
                ? perr.errorString()
                : QStringLiteral("expected a JSON array");
        }
        return {};
    }

    QList<ProjectInfo> projects;
    const QJsonArray arr = doc.array();
    projects.reserve(arr.size());
    for (const QJsonValue &v : arr)
        projects << projectFromJson(v.toObject());
    return projects;
}

namespace {

// Общий каркас запуска hgs: считает вывод только по finished (частичные readyRead
// не парсим — printf в hgs пишет одним куском, но QProcess вправе доставить его частями),
// бьёт по таймауту и всегда сообщает, какой бинарь и с какими аргументами не удался.
// Polls start hgs about once a second, so fork/exec runs on ProcessRunner's worker
// instead of the GUI thread. ProcessRunner also reports exactly once: the finished()
// that follows a timeout kill() must not reach onFailed() again (--selftest leaves
// main() on the first failure).
void runHgs(const QString &hgs, const QStringList &args, int timeoutMs, QObject *parent,
            const std::function<void(const QByteArray &)> &onFinished,
            const std::function<void(const QString &, const QString &)> &onFailed,
            const QByteArray &input = {})
{
    auto request = processRequest(hgs, args); request.timeoutMs = timeoutMs;
    if (!input.isEmpty()) request.input = input;
    ProcessRunner::run(std::move(request), parent, [hgs, args, onFinished, onFailed](const ProcessRunner::Result &result) {
        const QString what = hgs + QLatin1Char(' ') + args.join(QLatin1Char(' '));
        if (result.outcome == ProcessRunner::Result::TimedOut)
            return onFailed(what, QStringLiteral("timed out"));
        if (result.outcome == ProcessRunner::Result::FailedToStart)
            return onFailed(what, QStringLiteral("cannot start '%1': %2").arg(hgs, result.errorString));
        // hgs всегда возвращает 0 для "спящего пира" — ненулевой код или CrashExit
        // означает, что сломан сам вызов, а не то, что застали пира офлайн.
        if (result.exitStatus != QProcess::NormalExit || result.exitCode != 0) {
            const QString detail = QString::fromLocal8Bit(result.standardError).trimmed().left(4000);
            return onFailed(what, detail.isEmpty() ? QStringLiteral("exit code %1").arg(result.exitCode) : detail);
        }
        onFinished(result.standardOutput);
    });
}

} // namespace

void HgsClient::requestVersion()
{
    if (m_versionInFlight) return;
    m_versionInFlight=true;
    runHgs(m_hgs,{QStringLiteral("--version")},3000,this,
        [this](const QByteArray &out){
            m_versionInFlight=false;
            static const QRegularExpression format(QStringLiteral("^hgs ([0-9]+\\.[0-9]+\\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?)$"));
            const auto match=format.match(QString::fromUtf8(out).trimmed());
            emit versionReady(out.size()<=128 && match.hasMatch()?match.captured(1):QString{});
        },[this](const QString &,const QString &){m_versionInFlight=false;emit versionReady({});});
}

void HgsClient::requestSessionReference(const QString &host, const QString &name, const QString &run,
                                       const QString &conversation, const QString &archive, const QString &agent)
{
    QStringList args{"reference", name, "--json", "--run", run, "--conversation", conversation};
    if (!host.isEmpty()) args.prepend('@' + host);
    if (!archive.isEmpty()) args << "--archive" << archive;
    if (!agent.isEmpty()) args << "--agent" << agent;
    runHgs(m_hgs,args,host.isEmpty()?15000:25000,this,[this](const QByteArray &out){
        QJsonParseError error;const auto data=QJsonDocument::fromJson(out,&error);
        const auto uri=data.object().value("uri").toString();
        if(error.error!=QJsonParseError::NoError || !data.isObject() || !uri.startsWith("zerus://session?") || uri.size()>8192)
            emit sessionReferenceFailed(tr("Could not read a session reference. Update hgs on this computer."));
        else emit sessionReferenceReady(uri);
    },[this](const QString &,const QString &error){emit sessionReferenceFailed(error);});
}

quint64 HgsClient::requestAccounts(const QString &host, const QStringList &arguments, const QByteArray &input)
{
    const quint64 request = ++m_accountRequest;
    QStringList args;
    if (!host.isEmpty()) args << QLatin1Char('@') + host;
    args << QStringLiteral("account"); args.append(arguments);
    runHgs(m_hgs, args, arguments.value(0) == "set-key" ? 90000 : arguments.value(0) == "copy" ? 60000 : 25000, this,
        [this, request, host](const QByteArray &bytes) {
            QJsonParseError error; const auto data = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !data.isObject()) emit accountsFailed(request, host, tr("Invalid account catalog response"));
            else emit accountsReady(request, host, data.object());
        }, [this, request, host](const QString &, const QString &error) { emit accountsFailed(request, host, error); }, input);
    return request;
}

quint64 HgsClient::requestMachines(const QStringList &arguments)
{
    const quint64 request = ++m_machineRequest;
    QStringList args{QStringLiteral("machine")}; args.append(arguments);
    runHgs(m_hgs, args, arguments.value(0) == "check" ? 25000 : 8000, this,
        [this, request](const QByteArray &bytes) {
            QJsonParseError error; const auto data = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !data.isObject()) emit machinesFailed(request, tr("Invalid machine settings response"));
            else emit machinesReady(request, data.object());
        }, [this, request](const QString &, const QString &error) { emit machinesFailed(request, error); });
    return request;
}

quint64 HgsClient::requestSwarm(const QStringList &arguments, const QJsonObject &input)
{
    const quint64 request = ++m_swarmRequest;
    QStringList args{QStringLiteral("swarm")}; args.append(arguments);
    const bool network = QStringList{"preview", "join", "sync", "bind"}.contains(arguments.value(0));
    runHgs(m_hgs, args, network ? 180000 : 10000, this,
        [this, request](const QByteArray &bytes) {
            QJsonParseError error; const auto data = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !data.isObject()) emit swarmFailed(request, tr("Invalid project catalog response"));
            else emit swarmReady(request, data.object());
        }, [this, request](const QString &, const QString &error) { emit swarmFailed(request, error); },
        input.isEmpty() ? QByteArray() : QJsonDocument(input).toJson(QJsonDocument::Compact));
    return request;
}

void HgsClient::setPresence(double idleSeconds, bool locked)
{
    if (m_presenceInFlight) { m_pendingPresence = qMakePair(idleSeconds, locked); m_presencePending = true; return; }
    m_presenceInFlight = true;
    const auto input = QJsonDocument(QJsonObject{{"idle_seconds", qMax(0.0, idleSeconds)}, {"locked", locked}}).toJson(QJsonDocument::Compact);
    const auto finished = [this](const QString &error) {
        m_presenceInFlight = false;
        emit presenceFinished(error);
        if (std::exchange(m_presencePending, false)) setPresence(m_pendingPresence.first, m_pendingPresence.second);
    };
    runHgs(m_hgs, {QStringLiteral("presence"), QStringLiteral("set"), QStringLiteral("--json")}, 5000, this,
        [finished](const QByteArray &) { finished({}); },
        [finished](const QString &, const QString &detail) { finished(detail); }, input);
}

void HgsClient::setupMachine(const QString &alias, const QString &source)
{
    if (m_machineSetupInFlight) return;
    m_machineSetupInFlight = true;
    auto *proc = new QProcess(this); QStringList args{"machine", "setup", alias};
    if (!source.isEmpty()) args << "--source" << source;
    configureProcess(proc, m_hgs, args); proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]() {
        emit machineSetupOutput(QString::fromUtf8(proc->readAllStandardOutput()));
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_machineSetupInFlight = false; emit machineSetupFinished(false, proc->errorString()); proc->deleteLater();
    });
    connect(proc, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this, proc](int code, QProcess::ExitStatus status) {
        emit machineSetupOutput(QString::fromUtf8(proc->readAllStandardOutput()));
        m_machineSetupInFlight = false;
        const bool ok = code == 0 && status == QProcess::NormalExit;
        emit machineSetupFinished(ok, ok ? QString() : status == QProcess::CrashExit ? tr("Setup was interrupted. See the log above.") : tr("Setup exited with code %1. See the log above.").arg(code));
        proc->deleteLater();
    });
    proc->start();
}

void HgsClient::requestLocal()
{
    if (m_localInFlight) return;
    m_localInFlight = true;
    emit stateReadStarted({});
    // Large native histories can take longer than five seconds. Keep one poll
    // in flight and allow it to finish instead of continually killing/restarting it.
    runHgs(m_hgs, {QStringLiteral("ls"), QStringLiteral("--json"), QStringLiteral("--local")},
           15000, this,
           [this](const QByteArray &out) {
               m_localInFlight = false;
               QString err;
               const BoxState box = parseBox(out, &err);
               if (!err.isEmpty()) {
                   emit stateReadFailed(QString(), err);
                   emit failed(m_hgs + QStringLiteral(" ls --json --local"), err);
                   return;
               }
               emit localReady(box);
           },
           [this](const QString &what, const QString &detail) { m_localInFlight = false; emit stateReadFailed(QString(), detail); emit failed(what, detail); });
}

void HgsClient::requestPeer(const QString &peer)
{
    // Не копим запросы к одному и тому же пиру: пока предыдущий "@peer ls --json" не
    // завершился (спящий ноут держит ssh-рукопожатие все 15с таймаута), повторные вызовы
    // -- тик меню, минутный фон, "Refresh" -- просто игнорируются. Иначе десять открытий
    // меню подряд = десять параллельных ssh к боксу, который не отвечает ни на один.
    if (m_peerInFlight.contains(peer))
        return;
    m_peerInFlight.insert(peer);
    emit stateReadStarted(peer);

    // ssh-рукопожатие может занять больше времени, чем локальный вызов — у самого hgs
    // уже есть ConnectTimeout=3, но обвязка (auth, motd) укладывается не всегда в 5с.
    runHgs(m_hgs, {QLatin1Char('@') + peer, QStringLiteral("ls"), QStringLiteral("--json")},
           15000, this,
           [this, peer](const QByteArray &out) {
               // Снятие метки здесь и в onFailed ниже -- ровно те же два колбэка, которыми
               // runHgs() гарантированно завершает вызов РОВНО один раз (см. reported в
               // runHgs). Без этого упавший или зависший по таймауту пир навсегда
               // застревал бы в m_peerInFlight, и его строка в меню замерла бы на
               // "polling…" до перезапуска трея.
               m_peerInFlight.remove(peer);
               QString err;
               const BoxState box = parseBox(out, &err);
               if (!err.isEmpty()) {
                   emit stateReadFailed(peer, err);
                   emit failed(m_hgs + QStringLiteral(" ls --json"), err);
                   return;
               }
               emit peerReady(box);
           },
           [this, peer](const QString &what, const QString &detail) {
               m_peerInFlight.remove(peer);
               emit stateReadFailed(peer, detail);
               emit failed(what, detail);
           });
}

quint64 HgsClient::requestSearch(const QString &host, const QString &query)
{
    const auto id = ++m_searchRequest;
    QStringList args{"search", "--query", query, "--limit", "100"};
    if (!host.isEmpty()) args.prepend('@' + host);
    runHgs(m_hgs, args, host.isEmpty() ? 12000 : 20000, this,
        [this, id, host](const QByteArray &out) {
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(out, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject() || !doc.object().value("results").isArray())
                emit searchFailed(id, host, tr("Invalid search response; update hgs on this machine."));
            else emit searchReady(id, host, doc.object());
        }, [this, id, host](const QString &, const QString &error) { emit searchFailed(id, host, error); });
    return id;
}

quint64 HgsClient::requestProcess(const QString &host, const QString &name, const QString &processId, const QString &run,
                                const QString &conversation, const QString &archive, const QString &generation, bool stop)
{
    static quint64 counter=0;
    const auto request=++counter;
    QStringList args{"processes",name,stop?"--stop":"--output",processId,"--run",run,"--conversation",conversation};
    if(!archive.isEmpty())args<<"--archive"<<archive;
    if(!generation.isEmpty())args<<"--generation"<<generation;
    if(!host.isEmpty())args.prepend('@'+host);
    runHgs(m_hgs,args,45000,this,[this,request](const QByteArray &out){
        QJsonParseError error;const auto doc=QJsonDocument::fromJson(out,&error);
        if(error.error!=QJsonParseError::NoError || !doc.isObject())emit processFinished(request,{},tr("Invalid process response"));
        else emit processFinished(request,doc.object(),{});
    },[this,request](const QString &,const QString &error){emit processFinished(request,{},error);});
    return request;
}

bool HgsClient::requestInspection(const QString &host, const QString &name, qint64 after, const QString &archiveId, bool includeProcesses)
{
    const QString key = host + QLatin1Char('\n') + name + QLatin1Char('\n') + archiveId;
    if (m_inspectionsInFlight.contains(key))
        return false;
    m_inspectionsInFlight.insert(key);
    emit inspectionLoadingChanged(host, name, archiveId, {}, true);
    QStringList args{QStringLiteral("inspect"), name, QStringLiteral("--after"), QString::number(after)};
    if (!includeProcesses) args << QStringLiteral("--skip-processes");
    if (!archiveId.isEmpty())
        args << QStringLiteral("--archive") << archiveId;
    if (!host.isEmpty())
        args.prepend(QLatin1Char('@') + host);
    runHgs(m_hgs, args, host.isEmpty() ? 5000 : 15000, this,
           [this, host, name, key, archiveId](const QByteArray &out) {
               m_inspectionsInFlight.remove(key);
               emit inspectionLoadingChanged(host, name, archiveId, {}, false);
               QJsonParseError error;
               const auto doc = QJsonDocument::fromJson(out, &error);
               if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                   emit inspectionFailed(host, name, tr("Invalid activity response"), archiveId);
                   return;
               }
               emit inspectionReady(host, name, doc.object(), archiveId);
           }, [this, host, name, key, archiveId](const QString &, const QString &detail) {
               m_inspectionsInFlight.remove(key);
               emit inspectionLoadingChanged(host, name, archiveId, {}, false);
               emit inspectionFailed(host, name, detail, archiveId);
           });
    return true;
}

quint64 HgsClient::downloadFile(const QString &host, const QString &path, const QString &destination)
{
    const auto id = ++m_fileDownloadRequest;
    runHgs(m_hgs, {'@' + host, "download", "--path", path, "--output", destination}, 135000, this,
        [this, id, host, path, destination](const QByteArray &out) {
            if (out.size() > 65536) { emit fileDownloadFailed(id, tr("Invalid file transfer response.")); return; }
            const auto object = QJsonDocument::fromJson(out).object();
            const QFileInfo saved(destination);
            if (object.value("host").toString() != host || object.value("path").toString() != path
                || object.value("local_path").toString() != destination || !object.value("bytes").isDouble()
                || !LocalFile::readable(saved) || object.value("bytes").toDouble() != LocalFile::size(saved)) {
                emit fileDownloadFailed(id, tr("The file transfer did not confirm the saved local file.")); return;
            }
            emit fileDownloaded(id, destination);
        }, [this, id](const QString &, const QString &error) {
            emit fileDownloadFailed(id, error.startsWith("hgs: ") ? error.mid(5) : error);
        });
    return id;
}

quint64 HgsClient::requestAttachment(const QString &host, const QString &name, const QJsonObject &file,
                                     const QString &conversation, const QString &archive, const QString &agent)
{
    const auto id=++m_attachmentRequest;
    QStringList args{"attachment",name,"--request",file.value("request_id").toString(),"--index",QString::number(file.value("index").toInt(-1)),"--conversation",conversation};
    if(!archive.isEmpty())args<<"--archive"<<archive;
    if(!agent.isEmpty())args<<"--agent"<<agent;
    if(!host.isEmpty())args.prepend('@'+host);
    runHgs(m_hgs,args,host.isEmpty()?10000:45000,this,[this,id,file](const QByteArray &out){
        if(out.size()>MaximumAttachmentBytes*4/3+8192){emit attachmentFailed(id,tr("Attachment response is too large."));return;}
        const auto object=QJsonDocument::fromJson(out).object();
        const auto encoded=object.value("data_base64").toString().toLatin1();
        const auto decoded=QByteArray::fromBase64Encoding(encoded,QByteArray::AbortOnBase64DecodingErrors);
        if(out.size()>MaximumAttachmentBytes*4/3+8192 || object.value("request_id")!=file.value("request_id")
            || object.value("index")!=file.value("index") || !decoded || decoded.decoded.isEmpty() || decoded.decoded.size()>MaximumAttachmentBytes) {
            emit attachmentFailed(id,tr("Invalid attachment response. Refresh Activity and try again."));return;
        }
        auto metadata=object;metadata.remove("data_base64");emit attachmentReady(id,metadata,decoded.decoded);
    },[this,id](const QString &,const QString &error){emit attachmentFailed(id,error);});
    return id;
}

void HgsClient::launchNativeSession(const QString &host, const QString &target, const QString &name, const QString &account, const QString &launchId)
{
    launchDetachedSession(host,"dsh",target,name,account,launchId);
}

void HgsClient::launchDetachedSession(const QString &host, const QString &agent, const QString &target, const QString &name, const QString &account, const QString &launchId, const QString &model)
{
    QStringList args{agent, target, "--new", "-n", name, "--launch-id", launchId};
    if (agent != "dsh" && agent != "opencode") args << "-d";
    if (!account.isEmpty()) args << "--account" << account;
    if (agent == "opencode" && !model.isEmpty()) args << "--model" << model;
    if (!host.isEmpty()) args.prepend('@' + host);
    runHgs(m_hgs, args, 90000, this,
        [this,launchId](const QByteArray &) { emit nativeSessionLaunched(launchId,true,{}); },
        [this,launchId](const QString &,const QString &error) { emit nativeSessionLaunched(launchId,false,error); });
}

void HgsClient::requestSubagentInspection(const QString &host, const QString &name, const QString &agentId, const QString &archiveId)
{
    const QString key = host + '\n' + name + '\n' + archiveId + '\n' + agentId;
    if (m_inspectionsInFlight.contains(key)) return;
    m_inspectionsInFlight.insert(key);
    emit inspectionLoadingChanged(host, name, archiveId, agentId, true);
    QStringList args{"inspect", name, "--agent", agentId, "--skip-processes"};
    if (!archiveId.isEmpty()) args << "--archive" << archiveId;
    if (!host.isEmpty()) args.prepend('@' + host);
    runHgs(m_hgs, args, host.isEmpty() ? 5000 : 15000, this,
        [this,host,name,agentId,archiveId,key](const QByteArray &out) {
            m_inspectionsInFlight.remove(key);
            emit inspectionLoadingChanged(host, name, archiveId, agentId, false);
            QJsonParseError error; const auto doc = QJsonDocument::fromJson(out,&error);
            if (error.error != QJsonParseError::NoError || !doc.isObject())
                emit subagentInspectionFailed(host,name,agentId,archiveId,tr("Invalid subagent activity response"));
            else emit subagentInspectionReady(host,name,agentId,archiveId,doc.object());
        }, [this,host,name,agentId,archiveId,key](const QString &,const QString &error) {
            m_inspectionsInFlight.remove(key);
            emit inspectionLoadingChanged(host, name, archiveId, agentId, false);
            emit subagentInspectionFailed(host,name,agentId,archiveId,error);
        });
}

void HgsClient::runWrite(const QString &op, const QStringList &args, int timeoutMs,
                         bool sessionWrite, const QString &host)
{
    auto request = processRequest(m_hgs, args); request.timeoutMs = timeoutMs;
    ProcessRunner::run(std::move(request), this, [this, op, sessionWrite, host](const ProcessRunner::Result &result) {
        // Any QProcess error (failed start, crash) reports its own text, as the
        // errorOccurred() that precedes finished() did before.
        const bool timedOut = result.outcome == ProcessRunner::Result::TimedOut;
        const bool error = !timedOut && result.error != QProcess::UnknownError;
        const bool ok = !timedOut && !error && result.exitCode == 0 && result.exitStatus == QProcess::NormalExit;
        emit writeDone(op, ok, timedOut ? QStringLiteral("timed out") : error ? result.errorString
                                        : QString::fromLocal8Bit(result.standardError).trimmed());
        // A failed batch can have completed some sessions. Refresh its own machine
        // even on failure, without mixing it up with concurrent project edits.
        if (sessionWrite)
            emit sessionWriteDone(host);
    });
}

void HgsClient::forkSession(const QString &host, const QString &name, const QString &tag, const QString &archive, const QString &run, const QString &conversation)
{
    QStringList args{"fork", name, "-n", tag, "-d"};
    if (!archive.isEmpty()) args << "--archive" << archive;
    if (!run.isEmpty()) args << "--expected-run-id" << run;
    if (!conversation.isEmpty()) args << "--expected-conversation-id" << conversation;
    if (!host.isEmpty()) args.prepend('@' + host);
    runWrite(tr("fork %1").arg(name), args, host.isEmpty() ? 60000 : 90000, true, host);
}

void HgsClient::terminateSession(const QString &host, const QString &name, const QString &run)
{
    QStringList args{"terminate",name};
    if(!run.isEmpty())args<<"--expected-run-id"<<run;
    if(!host.isEmpty())args.prepend('@'+host);
    runWrite(QStringLiteral("terminate %1").arg(name),args,host.isEmpty()?15000:45000,true,host);
}

void HgsClient::killSession(const QString &host, const QString &name)
{
    const QString op = QStringLiteral("kill %1").arg(name);
    if (host.isEmpty()) {
        // Локальный kill — это exec tmux kill-session, доли миллисекунд: тот же бюджет,
        // что у requestLocal()/requestProjects().
        runWrite(op, {QStringLiteral("kill"), name}, 5000, true, host);
        return;
    }
    // Пиру -- через ssh (см. dispatch в hgs: kill получает ConnectTimeout=3, но не
    // ServerAliveInterval), поэтому бюджет как у requestPeer(): спящий ноут держит
    // рукопожатие до 15с, а не подвисает навсегда.
    runWrite(op, {QLatin1Char('@') + host, QStringLiteral("kill"), name}, 15000, true, host);
}

void HgsClient::openNativeUi(const QString &host, const QString &name)
{
    QStringList args{"native-ui",name}; if (!host.isEmpty()) args.prepend('@'+host);
    runWrite(tr("open native DeepSeek UI"),args,host.isEmpty()?45000:60000,false,host);
}

quint64 HgsClient::requestNativeUi(const QString &host, const QString &name)
{
    const auto request = ++m_nativeUiRequest;
    QStringList args{"native-ui", name, "--json"}; if (!host.isEmpty()) args.prepend('@' + host);
    runHgs(m_hgs, args, host.isEmpty() ? 45000 : 60000, this,
        [this, request](const QByteArray &out) {
            const auto doc = QJsonDocument::fromJson(out);
            if (!doc.isObject() || !doc.object().value("url").isString()) {
                emit nativeUiFailed(request, tr("Invalid native UI connection response")); return;
            }
            emit nativeUiReady(request, doc.object());
        }, [this, request](const QString &, const QString &error) { emit nativeUiFailed(request, error); });
    return request;
}

void HgsClient::pauseSession(const QString &host, const QString &name)
{
    QStringList args{QStringLiteral("pause"), name};
    if (!host.isEmpty())
        args.prepend(QLatin1Char('@') + host);
    // CLI waits up to 10s for a graceful exit, plus history verification and SSH.
    runWrite(QStringLiteral("pause %1").arg(name), args, 45000, true, host);
}

void HgsClient::resumeSession(const QString &host, const QString &name)
{
    QStringList args{QStringLiteral("resume"), name, QStringLiteral("-d")};
    if (!host.isEmpty())
        args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("resume %1").arg(name), args, 30000, true, host);
}

void HgsClient::startFreshSession(const QString &host, const QString &name, const QString &run)
{
    QStringList args{QStringLiteral("resume"), name, QStringLiteral("--fresh")};
    if (!run.isEmpty()) args << QStringLiteral("--expected-run-id") << run;
    args << QStringLiteral("-d");
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("fresh %1").arg(name), args, 30000, true, host);
}

void HgsClient::archiveSession(const QString &host, const QString &name)
{
    QStringList args{QStringLiteral("archive"), name};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("archive %1").arg(name), args, host.isEmpty() ? 5000 : 15000, true, host);
}

void HgsClient::renameSession(const QString &host, const QString &name, const QString &newName, const QString &archiveId)
{
    QStringList args{QStringLiteral("rename"), name, newName};
    if (!archiveId.isEmpty()) args << QStringLiteral("--archive") << archiveId;
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("rename %1").arg(name), args, host.isEmpty() ? 5000 : 15000, true, host);
}

void HgsClient::restoreArchive(const QString &host, const QString &name, const QString &archiveId)
{
    // Archive identity must never collapse to a generic resume of a reused name.
    if (archiveId.isEmpty()) {
        emit writeDone(QStringLiteral("restore %1").arg(name), false, tr("Missing archive identity"));
        return;
    }
    QStringList args{QStringLiteral("resume"), name, QStringLiteral("--archive"), archiveId, QStringLiteral("-d")};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("restore %1").arg(name), args, 30000, true, host);
}

void HgsClient::forgetArchive(const QString &host, const QString &name, const QString &archiveId)
{
    if (archiveId.isEmpty()) {
        emit writeDone(QStringLiteral("forget archive %1").arg(name), false, tr("Missing archive identity"));
        return;
    }
    QStringList args{QStringLiteral("kill"), name, QStringLiteral("--archive"), archiveId};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("forget archive %1").arg(name), args, host.isEmpty() ? 5000 : 15000, true, host);
}

void HgsClient::pauseAll(const QString &host, int count)
{
    QStringList args{QStringLiteral("pause"), QStringLiteral("--all")};
    if (!host.isEmpty())
        args.prepend(QLatin1Char('@') + host);
    // History verification and graceful exit are sequential for each session.
    runWrite(QStringLiteral("pause all on %1").arg(host.isEmpty() ? tr("this box") : host),
             args, 15000 + 30000 * qBound(1, count, 10000), true, host);
}

void HgsClient::resumeAll(const QString &host, int count)
{
    QStringList args{QStringLiteral("resume"), QStringLiteral("--all"), QStringLiteral("-d")};
    if (!host.isEmpty())
        args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("resume all on %1").arg(host.isEmpty() ? tr("this box") : host),
             args, 15000 + 15000 * qBound(1, count, 10000), true, host);
}

void HgsClient::projectSet(const QString &name, const QString &dir, const QString &host)
{
    const QString op = QStringLiteral("save project %1").arg(name);
    // projects.local — файл "одна запись на строку" (name=dir), который читает и
    // Ansible-оверлей, и резолвер проекта, и это же меню. hgs сам меняет каталог через
    // `cd "$dir" && pwd`, что молча пропускает перевод строки внутри пути насквозь, а
    // project_write() пишет его в файл как есть — запись обрывается на первом \n и
    // остаток превращается в мусорную "фантомную" строку без имени. Это порча самой
    // записи, которую пользователь только что сохранил, а не абстрактная гигиена —
    // поэтому чек здесь, до запуска процесса, а не патч в hgs (вне tray/ в этой
    // задаче, да и остальную валидацию имени hgs и так делает сам, дублировать незачем).
    if (dir.contains(QLatin1Char('\n')) || dir.contains(QLatin1Char('\r'))) {
        // Сигналим через singleShot(0), а не emit прямо здесь: writeDone для всех
        // остальных исходов приходит асинхронно, через цикл событий (процесс всегда
        // запускается и завершается позже) -- если этот единственный синхронный путь
        // прилетит раньше app.exec(), QCoreApplication::exit() из обработчика
        // окажется вызван до того, как цикл вообще стартовал, и тихо потеряется
        // (проверено: без singleShot ранер зависает до собственного канареечного
        // таймаута). singleShot(0) держит контракт "всегда через loop" одинаковым
        // для всех трёх исходов.
        QTimer::singleShot(0, this, [this, op]() {
            emit writeDone(op, false,
                            QStringLiteral("the path has a newline in it — it would corrupt "
                                            "projects.local"));
        });
        return;
    }
    QStringList args{QStringLiteral("project"), QStringLiteral("set"), name, dir};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(op, args, host.isEmpty() ? 5000 : 15000);
}

void HgsClient::projectRemove(const QString &name, const QString &host)
{
    QStringList args{QStringLiteral("project"), QStringLiteral("rm"), name};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runWrite(QStringLiteral("remove project %1").arg(name), args, host.isEmpty() ? 5000 : 15000);
}

void HgsClient::requestProjects(const QString &host)
{
    QStringList args{QStringLiteral("project"), QStringLiteral("ls"), QStringLiteral("--json")};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runHgs(m_hgs, args, host.isEmpty() ? 5000 : 15000, this,
           [this, host](const QByteArray &out) {
               QString err;
               const auto projects = parseProjects(out, &err);
               if (!err.isEmpty()) { emit projectsFailed(host, err); return; }
               if (host.isEmpty()) emit projectsReady(projects);
               emit projectsForHostReady(host, projects);
           }, [this, host](const QString &, const QString &detail) { emit projectsFailed(host, detail); });
}

namespace {
QString worktreeCacheKey(const QString &executable,const QString &host,const QString &path) {
    return QString::fromLatin1(QCryptographicHash::hash((executable+'\n'+host+'\n'+path).toUtf8(),QCryptographicHash::Sha256).toHex());
}
}
QJsonObject HgsClient::worktreeSnapshot(const QString &host,const QString &path) const
{
    const auto bytes=QSettings().value("workspace/worktreeSnapshots").toByteArray();
    if(bytes.size()>8*1024*1024)return {};
    return QJsonDocument::fromJson(bytes).object().value(worktreeCacheKey(m_hgs,host,path)).toObject();
}
QJsonObject HgsClient::gitStatusSnapshot(const QString &host, const QString &path) const
{
    return m_gitStatusCache.value(host + '\n' + path);
}

void HgsClient::requestGitStatus(const QString &host, const QString &path)
{
    if (path.isEmpty()) return;
    const auto key = host + '\n' + path;
    const auto age = QDateTime::currentMSecsSinceEpoch() - m_gitStatusRequestedAt.value(key);
    if (m_gitStatusRequestedAt.contains(key) && age >= 0 && age < 15000) return;
    if (m_gitStatusInFlight.contains(key) || m_gitStatusQueue.contains({host, path})) return;
    // Bound both subprocess concurrency and old folder snapshots.
    if (m_gitStatusQueue.size() >= 64) return;
    m_gitStatusQueue.append({host, path}); pumpGitStatus();
}

void HgsClient::pumpGitStatus()
{
    while (m_gitStatusInFlight.size() < 2 && !m_gitStatusQueue.isEmpty()) {
        const auto folder = m_gitStatusQueue.takeFirst();
        const auto host = folder.first, path = folder.second, key = host + '\n' + path;
        m_gitStatusInFlight.insert(key);
        const auto finish = [this, host, path, key](QJsonObject data) {
            const auto received = QDateTime::currentMSecsSinceEpoch();
            if (!data.value("state").isString()) data = {{"state", "unavailable"}};
            data["received_at_ms"] = received;
            m_gitStatusCache.insert(key, data); m_gitStatusRequestedAt.insert(key, received);
            m_gitStatusInFlight.remove(key);
            while (m_gitStatusCache.size() > 128) {
                QString oldest; qint64 at = std::numeric_limits<qint64>::max();
                for (auto i = m_gitStatusRequestedAt.cbegin(); i != m_gitStatusRequestedAt.cend(); ++i)
                    if (!m_gitStatusInFlight.contains(i.key()) && i.value() < at) { oldest = i.key(); at = i.value(); }
                m_gitStatusCache.remove(oldest); m_gitStatusRequestedAt.remove(oldest);
            }
            emit gitStatusChanged(host, path); pumpGitStatus();
        };
        QStringList args{"git-status", "--path", path, "--json"};
        if (!host.isEmpty()) args.prepend('@' + host);
        runHgs(m_hgs, args, host.isEmpty() ? 5000 : 10000, this,
            [finish](const QByteArray &out) { finish(out.size() <= 32768 ? QJsonDocument::fromJson(out).object() : QJsonObject()); },
            [finish](const QString &, const QString &) { finish({}); });
    }
}

quint64 HgsClient::requestWorktrees(const QString &host,const QString &path,bool refresh)
{
    const quint64 request=++m_worktreeRequest;
    const auto cached=worktreeSnapshot(host,path);
    const auto age=QDateTime::currentSecsSinceEpoch()-qint64(cached.value("sampled_at").toDouble());
    if(!refresh&&!cached.isEmpty()&&!cached.value("stale").toBool()&&age>=0&&age<30){
        QTimer::singleShot(0,this,[this,request,host,path,cached]{emit worktreesReady(request,host,path,cached);});return request;
    }
    const auto key=host+'\n'+path;
    if(m_worktreesInFlight.contains(key)){m_worktreesInFlight[key].append({request,path});return request;}
    m_worktreesInFlight[key].append({request,path});
    QStringList args{"worktrees","--path",path,"--json"};if(refresh)args<<"--refresh";if(!host.isEmpty())args.prepend('@'+host);
    runHgs(m_hgs,args,host.isEmpty()?6000:15000,this,[this,host,key](const QByteArray &out){
        const auto requests=m_worktreesInFlight.take(key);
        const auto data=out.size()<=2*1024*1024?QJsonDocument::fromJson(out).object():QJsonObject();
        if(!data.value("worktrees").isArray()||!data.value("state").isString()){
            for(const auto &r:requests)emit worktreesFailed(r.first,host,r.second,tr("Invalid worktree catalog response"));return;
        }
        QSettings settings;auto snapshots=QJsonDocument::fromJson(settings.value("workspace/worktreeSnapshots").toByteArray()).object();
        auto store=[&](const QString &path,QJsonObject value){snapshots[worktreeCacheKey(m_hgs,host,path)]=value;};
        for(const auto &r:requests)store(r.second,data);
        if(data["state"]=="ok")for(const auto &v:data["worktrees"].toArray()){
            const auto root=v.toObject()["path"].toString();auto sibling=data;sibling["path"]=root;sibling["selected_root"]=root;store(root,sibling);
        }
        while(snapshots.size()>64||QJsonDocument(snapshots).toJson(QJsonDocument::Compact).size()>8*1024*1024){QString oldest;double time=std::numeric_limits<double>::max();for(auto i=snapshots.begin();i!=snapshots.end();++i)if(i.value().toObject()["sampled_at"].toDouble()<time){oldest=i.key();time=i.value().toObject()["sampled_at"].toDouble();}snapshots.remove(oldest);}
        settings.setValue("workspace/worktreeSnapshots",QJsonDocument(snapshots).toJson(QJsonDocument::Compact));
        for(const auto &r:requests)emit worktreesReady(r.first,host,r.second,data);
    },[this,host,key](const QString &,const QString &detail){for(const auto &r:m_worktreesInFlight.take(key))emit worktreesFailed(r.first,host,r.second,detail);});
    return request;
}

quint64 HgsClient::createWorktree(const QString &host,const QString &path,const QString &commonDir,
    const QString &branch,const QString &base,const QString &destination)
{
    const auto request=++m_worktreeRequest;
    const auto token=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList args{"worktrees","create","--path",path,"--common-dir",commonDir,"--branch",branch,
        "--base",base,"--destination",destination,"--request-id",token,"--json"};
    if(!host.isEmpty())args.prepend('@'+host);
    runHgs(m_hgs,args,330000,this,[this,request,token,branch,commonDir](const QByteArray &out){
        const auto data=QJsonDocument::fromJson(out).object();
        if(data["request_id"]!=token||data["status"]!="created"||data["branch"]!=branch||data["common_dir"]!=commonDir
            ||!QFileInfo(data["path"].toString()).isAbsolute()){
            emit worktreeCreated(request,false,{},tr("Creation could not be confirmed. Refresh the catalog and check the destination before retrying."));return;
        }
        QSettings settings;auto snapshots=QJsonDocument::fromJson(settings.value("workspace/worktreeSnapshots").toByteArray()).object();
        for(auto i=snapshots.begin();i!=snapshots.end();++i){auto value=i.value().toObject();
            if(value["common_dir"]==commonDir&&value["machine"]==data["machine"]){value["stale"]=true;i.value()=value;}}
        settings.setValue("workspace/worktreeSnapshots",QJsonDocument(snapshots).toJson(QJsonDocument::Compact));
        emit worktreeCreated(request,true,data,{});
    },[this,request,destination](const QString &,const QString &error){
        emit worktreeCreated(request,false,{},error+tr("\nCheck %1 before retrying. Any created worktree is kept.").arg(destination));
    });
    return request;
}

namespace {
QString cleanupError(const QString &host,const QString &detail) {
    // An older hgs prints the catalog usage for the new review/remove actions.
    if(detail.contains(QStringLiteral("usage: hgs worktrees --path")))
        return host.isEmpty()?HgsClient::tr("Update Zerus on this computer to clean up worktrees."):HgsClient::tr("Update Zerus on %1 to clean up worktrees.").arg(host);
    return detail;
}
}
quint64 HgsClient::reviewWorktrees(const QString &host,const QString &path,const QString &worktree)
{
    const auto request=++m_worktreeRequest;const auto key=host+'\n'+path+'\n'+worktree;
    if(m_reviewsInFlight.contains(key)){m_reviewsInFlight[key].append(request);return request;}
    m_reviewsInFlight[key].append(request);
    QStringList args{"worktrees","review","--path",path,"--json"};if(!worktree.isEmpty())args<<"--worktree"<<worktree;if(!host.isEmpty())args.prepend('@'+host);
    runHgs(m_hgs,args,host.isEmpty()?30000:45000,this,[this,host,path,key,worktree](const QByteArray &out){
        const auto requests=m_reviewsInFlight.take(key);
        const auto data=out.size()<=4*1024*1024?QJsonDocument::fromJson(out).object():QJsonObject();
        if(!data.value("worktrees").isArray()||!data.value("state").isString()){for(const auto r:requests)emit worktreeReviewFailed(r,host,path,tr("Invalid worktree review response"));return;}
        if(worktree.isEmpty()&&data["state"]=="ok")m_worktreeReviews.insert(host+'\n'+data["common_dir"].toString(),data);
        for(const auto r:requests)emit worktreeReviewReady(r,host,path,data);
    },[this,host,path,key](const QString &,const QString &detail){for(const auto r:m_reviewsInFlight.take(key))emit worktreeReviewFailed(r,host,path,cleanupError(host,detail));});
    return request;
}
QJsonObject HgsClient::worktreeReview(const QString &host,const QString &commonDir) const
{
    const auto data=m_worktreeReviews.value(host+'\n'+commonDir);
    const auto age=QDateTime::currentSecsSinceEpoch()-qint64(data.value("sampled_at").toDouble());
    return age>=0&&age<300?data:QJsonObject();
}
quint64 HgsClient::removeWorktree(const QString &host,const QString &path,const QString &commonDir,const QString &fingerprint,bool deleteBranch)
{
    const auto request=++m_worktreeRequest;const auto token=QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList args{"worktrees","remove","--path",path,"--common-dir",commonDir,"--fingerprint",fingerprint,"--request-id",token,"--json"};
    if(deleteBranch)args<<"--delete-branch";if(!host.isEmpty())args.prepend('@'+host);
    const auto forget=[this,host,commonDir]{
        // Removal changes the repository whatever the outcome; never trust old verdicts.
        m_worktreeReviews.remove(host+'\n'+commonDir);
        QSettings settings;auto snapshots=QJsonDocument::fromJson(settings.value("workspace/worktreeSnapshots").toByteArray()).object();
        for(auto i=snapshots.begin();i!=snapshots.end();++i){auto value=i.value().toObject();if(value["common_dir"]==commonDir){value["stale"]=true;i.value()=value;}}
        settings.setValue("workspace/worktreeSnapshots",QJsonDocument(snapshots).toJson(QJsonDocument::Compact));
    };
    runHgs(m_hgs,args,330000,this,[this,request,token,path,forget](const QByteArray &out){
        forget();const auto data=QJsonDocument::fromJson(out).object();
        if(data["request_id"]!=token||(data["status"]!="removed"&&data["status"]!="forgotten")||data["path"]!=path){
            emit worktreeRemoved(request,false,{},tr("Removal could not be confirmed. Review the worktrees again."));return;
        }
        emit worktreeRemoved(request,true,data,{});
    },[this,request,host,forget](const QString &,const QString &error){forget();emit worktreeRemoved(request,false,{},cleanupError(host,error));});
    return request;
}

quint64 HgsClient::requestDirectories(const QString &host, const QString &path, bool hidden)
{
    const quint64 request = ++m_directoryRequest;
    QStringList args{QStringLiteral("dirs")};
    if (hidden) args << QStringLiteral("--hidden");
    args << (path.isEmpty() ? QStringLiteral("~") : path);
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    runHgs(m_hgs, args, host.isEmpty() ? 5000 : 15000, this,
           [this, host, request](const QByteArray &out) {
               QJsonParseError error;
               const auto doc = QJsonDocument::fromJson(out, &error);
               if (!doc.isObject() || !doc.object().value("path").isString()
                   || !doc.object().value("directories").isArray()) {
                   emit directoriesFailed(request, host, tr("Invalid directory response")); return;
               }
               emit directoriesReady(request, host, doc.object());
           }, [this, host, request](const QString &, const QString &detail) {
               emit directoriesFailed(request, host, detail);
           });
    return request;
}

quint64 HgsClient::requestAnswerQuestion(const QString &host, const QString &name,
                                        const QJsonObject &question, const QJsonArray &answers)
{
    const quint64 request = ++m_answerRequest;
    const QString key = host + QLatin1Char('\n') + name;
    const QString questionId = question.value("question_id").toString();
    const QString questionHash = question.value("question_hash").toString();
    const QString runId = question.value("run_id").toString();
    const QString conversationId = question.value("conversation_id").toString();
    const bool asyncQuestion = question.value("source") == "codex_async" && question.value("optional").toBool();
    // Native agents ask for folder trust before SessionStart creates a conversation.
    // Only recognized native startup requests may omit it; the CLI checks the
    // exact run, process, folder and complete disclosure before sending keys.
    const auto trustProvider = question.value("source") == "claude_folder_trust" ? QStringLiteral("claude")
        : question.value("source") == "codex_folder_trust" ? QStringLiteral("codex") : QStringLiteral("kimi");
    const bool startupTrust = question.value("source") == trustProvider + "_folder_trust"
        && question.value("answer_transport") == trustProvider + "_tui" && questionHash.size() == 64
        && questionId == trustProvider + "-trust:" + questionHash;
    const bool startupPermission = question.value("source") == "claude_permission_mode"
        && question.value("answer_transport") == "claude_tui" && questionHash.size() == 64
        && questionId == "claude-permissions:" + questionHash;
    const bool startupHooks = question.value("source") == "codex_hooks_trust"
        && question.value("answer_transport") == "codex_tui" && questionHash.size() == 64
        && questionId == "codex-hooks-trust:" + questionHash;
    const bool startupUpdate = question.value("source") == "codex_update"
        && question.value("answer_transport") == "codex_tui" && questionHash.size() == 64
        && questionId == "codex-update:" + questionHash;
    auto reject = [this, request, host, name](const QString &detail) {
        QTimer::singleShot(0, this, [this, request, host, name, detail] {
            emit questionAnswerFailed(request, host, name, detail, false);
        });
    };
    if (m_answersInFlight.contains(key)) {
        reject(tr("An answer to this session is already being sent.")); return request;
    }
    const bool canSkip = question.value("optional").toBool() && question.value("can_skip").toBool()
        && question.value("source")=="codex_async" && !answers.isEmpty()
        && std::all_of(answers.begin(),answers.end(),[](const QJsonValue &value){return value.toObject().value("skip").toBool();});
    if (name.isEmpty() || questionId.isEmpty() || questionHash.isEmpty() || runId.isEmpty()
        || (conversationId.isEmpty() && !startupTrust && !startupPermission && !startupHooks && !startupUpdate) || answers.isEmpty() || (!question.value("can_answer").toBool() && !canSkip)) {
        reject(tr("This request is not ready to answer. Wait for an updated request or open Terminal.")); return request;
    }
    const QString program = resolvedProgram(m_hgs);
    const QFileInfo executable(program);
    if (program.isEmpty() || !executable.isFile() || !executable.isExecutable()) {
        reject(tr("Cannot start hgs: executable not found or not executable: %1").arg(m_hgs)); return request;
    }
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray payload = QJsonDocument(QJsonObject{
        {"request_id", requestId}, {"question_id", questionId}, {"expected_question_hash", questionHash},
        {"expected_run_id", runId}, {"expected_conversation_id", conversationId}, {"answers", answers}
    }).toJson(QJsonDocument::Compact);
    if (payload.size() > MaximumMessageBytes) {
        reject(tr("The answers exceed 64 KiB.")); return request;
    }
    QStringList args{QStringLiteral("answer"), name, QStringLiteral("--json")};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    auto *process = new QProcess(this);
    configureProcess(process, program, args);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    auto fail = [this, request, host, name, key, reported](const QString &detail, bool uncertain) {
        if (*reported) return;
        *reported = true; m_answersInFlight.remove(key);
        emit questionAnswerFailed(request, host, name, detail, uncertain);
    };
    connect(process, &QProcess::started, process, [process, payload, fail] {
        if (process->write(payload) != payload.size()) {
            fail(tr("Answer transfer failed. Check Terminal before retrying."), true); process->kill();
        }
        process->closeWriteChannel();
    });
    connect(timer, &QTimer::timeout, process, [process, fail] {
        fail(tr("Answer delivery timed out. Check Terminal before retrying."), true); process->kill();
    });
    connect(process, &QProcess::errorOccurred, process, [process, timer, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            timer->stop(); fail(tr("Cannot start hgs: %1").arg(process->errorString()), false); process->deleteLater();
        }
    });
    connect(process, &QProcess::finished, process,
        [this, process, timer, fail, reported, request, host, name, key, requestId, questionId, questionHash, runId, conversationId, asyncQuestion, canSkip]
        (int code, QProcess::ExitStatus exitStatus) {
            timer->stop();
            if (*reported) { process->deleteLater(); return; }
            const QString errorText = QString::fromUtf8(process->readAllStandardError()).trimmed().left(4000);
            if (code != 0 || exitStatus != QProcess::NormalExit) {
                const bool uncertain = exitStatus != QProcess::NormalExit || code != 1
                    || errorText.contains(QStringLiteral("delivery uncertain"), Qt::CaseInsensitive);
                fail(errorText.isEmpty() ? tr("Answer transfer failed (exit %1).").arg(code) : errorText, uncertain);
            } else {
                QJsonParseError error;
                const auto doc = QJsonDocument::fromJson(process->readAllStandardOutput(), &error);
                const auto receipt = doc.object();
                if (error.error != QJsonParseError::NoError || !doc.isObject()
                    || (receipt.value("status") != "answered" && !(asyncQuestion && !canSkip && receipt.value("status") == "submitted" && !receipt.value("skipped").toBool()))
                    || receipt.value("request_id").toString() != requestId
                    || receipt.value("name").toString() != name
                    || receipt.value("question_id").toString() != questionId
                    || receipt.value("question_hash").toString() != questionHash
                    || receipt.value("run_id").toString() != runId
                    || !receipt.value("conversation_id").isString()
                    || receipt.value("conversation_id").toString() != conversationId)
                    fail(tr("The answer acknowledgement is invalid. Check Terminal before retrying."), true);
                else {
                    *reported = true; m_answersInFlight.remove(key);
                    if (receipt.value("status") == "submitted") emit questionAnswerSubmitted(request, host, name, receipt);
                    else emit questionAnswered(request, host, name, receipt);
                    emit sessionWriteDone(host);
                }
            }
            process->deleteLater();
        });
    m_answersInFlight.insert(key);
    timer->start(host.isEmpty() ? 30000 : 45000);
    process->start(); return request;
}

quint64 HgsClient::requestSendMessage(const QString &host, const QString &name, const QString &text,
                                     const QList<MessageAttachment> &attachments,
                                     const QString &expectedRunId, const QString &expectedConversationId, const QString &agentId, const QString &compactionId)
{
    const quint64 request = ++m_messageRequest;
    const QString key = host + QLatin1Char('\n') + name + '\n' + agentId;
    auto reject = [this, request, host, name](const QString &detail) {
        QTimer::singleShot(0, this, [this, request, host, name, detail]() {
            emit messageFailed(request, host, name, detail, false);
        });
    };
    if (m_messagesInFlight.contains(key)) {
        reject(tr("A message to this session is already being sent."));
        return request;
    }
    if (name.isEmpty() || expectedRunId.isEmpty()) {
        reject(tr("Refresh the session before sending a message."));
        return request;
    }
    // On macOS configureProcess starts a shell trampoline before exec'ing hgs.
    // A missing CLI would otherwise look like a delivery-uncertain exit127.
    // Detect definite startup failures before passing any message bytes; an
    // actual executable exiting127 must still remain uncertain.
    const QString program = resolvedProgram(m_hgs);
    const QFileInfo executable(program);
    if (program.isEmpty() || !executable.isFile() || !executable.isExecutable()) {
        reject(tr("Cannot start hgs: executable not found or not executable: %1").arg(m_hgs));
        return request;
    }
    if (text.toUtf8().size() > MaximumMessageBytes || attachments.size() > MaximumAttachments) {
        reject(tr("Use at most 64 KiB of text and 8 attachments."));
        return request;
    }
    qsizetype total = 0;
    QJsonArray files;
    for (const MessageAttachment &attachment : attachments) {
        if (attachment.data.isEmpty() || attachment.data.size() > MaximumAttachmentBytes) {
            reject(tr("Each attachment must be nonempty and at most 10 MiB."));
            return request;
        }
        total += attachment.data.size();
        if (total > MaximumAttachmentsBytes) {
            reject(tr("Attachments exceed 20 MiB in total."));
            return request;
        }
        QJsonObject file{{"name", attachment.name}, {"mime", attachment.mime},
            {"data_base64", QString::fromLatin1(attachment.data.toBase64())}};
        if (!attachment.reference.isEmpty()) file["reference"] = attachment.reference;
        files.append(file);
    }
    if (text.trimmed().isEmpty() && files.isEmpty()) {
        reject(tr("Write a message or attach a file first."));
        return request;
    }
    const QString messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject message{
        {"request_id", messageId}, {"text", text}, {"attachments", files},
        {"expected_run_id", expectedRunId}, {"expected_conversation_id", expectedConversationId}
    };
    if (!agentId.isEmpty()) message["agent_id"] = agentId;
    if (!compactionId.isEmpty()) message["expected_compaction_id"] = compactionId;
    const auto payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QStringList args{QStringLiteral("send"), name, QStringLiteral("--json")};
    if (!host.isEmpty()) args.prepend(QLatin1Char('@') + host);
    auto *process = new QProcess(this);
    configureProcess(process, program, args);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    auto fail = [this, request, host, name, key, reported](const QString &detail, bool uncertain) {
        if (*reported) return;
        *reported = true;
        m_messagesInFlight.remove(key);
        emit messageFailed(request, host, name, detail, uncertain);
    };
    connect(process, &QProcess::started, process, [process, payload, fail]() {
        // The request and file bytes never appear in argv, a shell command, or a
        // clipboard. closeWriteChannel drains QProcess's pending bytes before EOF.
        if (process->write(payload) != payload.size()) {
            fail(tr("Message transfer failed. Inspect Terminal before retrying."), true);
            process->kill();
        }
        process->closeWriteChannel();
    });
    connect(timer, &QTimer::timeout, process, [process, fail]() {
        fail(tr("Delivery timed out. The message may have reached the agent; inspect Terminal before retrying."), true);
        process->kill();
    });
    connect(process, &QProcess::errorOccurred, process,
        [process, timer, fail](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                timer->stop();
                fail(tr("Cannot start hgs: %1").arg(process->errorString()), false);
                process->deleteLater();
            }
        });
    connect(process, &QProcess::finished, process,
        [this, process, timer, fail, reported, request, host, name, key, messageId, expectedRunId, expectedConversationId, agentId]
        (int code, QProcess::ExitStatus status) {
            timer->stop();
            if (*reported) { process->deleteLater(); return; }
            const QString detail = QString::fromUtf8(process->readAllStandardError()).trimmed().left(4000);
            if (code != 0 || status != QProcess::NormalExit) {
                const bool uncertain = status != QProcess::NormalExit || code != 1
                    || detail.contains(QStringLiteral("delivery uncertain"), Qt::CaseInsensitive);
                fail(detail.isEmpty() ? tr("Message transfer failed (exit %1).").arg(code) : detail, uncertain);
            } else {
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(process->readAllStandardOutput(), &error);
                const QJsonObject receipt = document.object();
                if (error.error != QJsonParseError::NoError || !document.isObject()
                    || receipt.value("status").toString() != QLatin1String("submitted")
                    || receipt.value("request_id").toString() != messageId
                    || receipt.value("agent_id").toString() != agentId
                    || receipt.value("name").toString() != name
                    || receipt.value("run_id").toString() != expectedRunId
                    || !receipt.value("conversation_id").isString()
                    || receipt.value("conversation_id").toString() != expectedConversationId
                    || (expectedConversationId.isEmpty() && !receipt.value("first_message").toBool())) {
                    fail(tr("Delivery acknowledgement is invalid. Inspect Terminal before retrying."), true);
                } else {
                    *reported = true;
                    m_messagesInFlight.remove(key);
                    emit messageSent(request, host, name, receipt);
                    emit sessionWriteDone(host);
                }
            }
            process->deleteLater();
        });
    m_messagesInFlight.insert(key);
    timer->start(host.isEmpty() ? 15000 : 45000);
    process->start();
    return request;
}

quint64 HgsClient::requestStageFiles(const QString &host, const QString &name, const QStringList &paths,
                                     const QString &run, const QString &conversation)
{
    QJsonArray files; qsizetype total = 0; QString error;
    if (paths.isEmpty() || paths.size() > MaximumAttachments) error = tr("Drop between 1 and 8 files.");
    for (const auto &path : paths) {
        QFile file(path); const QFileInfo info(path);
        if (!error.isEmpty()) break;
        const auto size = LocalFile::size(info);
        if (!LocalFile::readable(info) || size <= 0 || size > MaximumAttachmentBytes || !file.open(QIODevice::ReadOnly)) {
            error = tr("Each file must be readable, nonempty, and at most 10 MiB."); break;
        }
        const auto bytes = file.read(MaximumAttachmentBytes + 1); total += bytes.size();
        if (bytes.isEmpty() || bytes.size() > MaximumAttachmentBytes || total > MaximumAttachmentsBytes) {
            error = tr("Files exceed 20 MiB in total or changed during transfer."); break;
        }
        files.append(QJsonObject{{"name", info.fileName()}, {"mime", QMimeDatabase().mimeTypeForFile(info).name()},
            {"data_base64", QString::fromLatin1(bytes.toBase64())}});
    }
    if (!error.isEmpty()) {
        const auto id = ++m_sessionActionRequest;
        QTimer::singleShot(0, this, [this,id,error] { emit sessionActionFinished(id,false,{},error); }); return id;
    }
    return runSessionAction(host,name,"attachment",{{"expected_run_id",run},{"expected_conversation_id",conversation},{"attachments",files}});
}

quint64 HgsClient::requestCompactContext(const QString &host, const QString &name, const QString &run, const QString &conversation)
{
    return runSessionAction(host,name,"compact-context",{{"expected_run_id",run},{"expected_conversation_id",conversation}});
}

quint64 HgsClient::requestClearContext(const QString &host, const QString &name, const QString &run, const QString &conversation, bool dismissPrompt)
{
    QJsonObject data{{"expected_run_id",run},{"expected_conversation_id",conversation}};
    if (dismissPrompt) data["dismiss_prompt"] = true; // Older hgs rejects the unknown field.
    return runSessionAction(host,name,"clear-context",data);
}

quint64 HgsClient::requestInterrupt(const QString &host, const QString &name, const QString &run,
                                    const QString &conversation, double turnStarted)
{
    return runSessionAction(host,name,"interrupt",{{"expected_run_id",run},{"expected_conversation_id",conversation},{"expected_turn_started",turnStarted}});
}

quint64 HgsClient::runSessionAction(const QString &host, const QString &name, const QString &operation, QJsonObject data)
{
    const auto request = ++m_sessionActionRequest;
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces); data["request_id"] = requestId;
    QStringList args{operation,name}; if (operation == "attachment") args << "--stage";
    args << "--json"; if (!host.isEmpty()) args.prepend('@' + host);
    auto *process = new QProcess(this); configureProcess(process,m_hgs,args);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    const auto fail = [this,request,reported](const QString &error) {
        if (*reported) return; *reported = true; emit sessionActionFinished(request,false,{},error);
    };
    const auto payload = QJsonDocument(data).toJson(QJsonDocument::Compact);
    connect(process,&QProcess::started,process,[process,payload,fail] {
        if (process->write(payload) != payload.size()) { fail(tr("Transfer failed. Check Terminal before retrying.")); process->kill(); }
        process->closeWriteChannel();
    });
    connect(timer,&QTimer::timeout,process,[process,fail] { fail(tr("Request timed out. Check Terminal before retrying.")); process->kill(); });
    connect(process,&QProcess::errorOccurred,process,[process,timer,fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { timer->stop(); fail(process->errorString()); process->deleteLater(); }
    });
    connect(process,&QProcess::finished,process,[this,process,timer,fail,reported,request,data,operation,name,host](int code,QProcess::ExitStatus status) {
        timer->stop();
        if (!*reported) {
            const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed().left(2000);
            if (code || status != QProcess::NormalExit) fail(error.isEmpty() ? tr("Could not complete the request. Check Terminal.") : error);
            else {
                const auto result = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
                if (result.value("request_id") != data.value("request_id") || result.value("name") != name
                    || result.value("run_id") != data.value("expected_run_id") || result.value("conversation_id") != data.value("expected_conversation_id")
                    || !(result.value("status") == (operation == "attachment" ? "staged" : "submitted")
                        || (operation == "clear-context" && result.value("status") == "confirmed")))
                    fail(tr("Invalid acknowledgement. Check Terminal before retrying."));
                else { *reported = true; emit sessionActionFinished(request,true,result,{}); if (operation != "attachment") emit sessionWriteDone(host); }
            }
        }
        process->deleteLater();
    });
    timer->start(host.isEmpty() ? 15000 : 45000); process->start(); return request;
}

quint64 HgsClient::requestQueueSendNow(const QString &host, const QString &name, const QString &run, const QString &conversation, const QString &queue)
{
    const auto request = ++m_messageRequest;
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto payload = QJsonDocument(QJsonObject{{"request_id", requestId}, {"expected_run_id", run},
        {"expected_conversation_id", conversation}, {"queue_id", queue}}).toJson(QJsonDocument::Compact);
    QStringList args{"send-now", name, "--json"}; if (!host.isEmpty()) args.prepend('@' + host);
    auto *process = new QProcess(this); configureProcess(process, m_hgs, args);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    const auto fail = [this, request, reported](const QString &error) {
        if (*reported) return; *reported = true; emit queueSendFinished(request, false, {}, error);
    };
    connect(process, &QProcess::started, process, [process, payload, fail] {
        if (process->write(payload) != payload.size()) { fail(tr("Queue action failed. Check Terminal before retrying.")); process->kill(); }
        process->closeWriteChannel();
    });
    connect(timer, &QTimer::timeout, process, [process, fail] {
        fail(tr("Queue action timed out. Check Terminal before retrying.")); process->kill();
    });
    connect(process, &QProcess::errorOccurred, process, [process, timer, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { timer->stop(); fail(process->errorString()); process->deleteLater(); }
    });
    connect(process, &QProcess::finished, process, [this, process, timer, fail, reported, request, requestId, name, run, conversation, queue, host](int code, QProcess::ExitStatus status) {
        timer->stop();
        if (!*reported) {
            if (code || status != QProcess::NormalExit) {
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed().left(2000);
                fail(error.isEmpty() ? tr("Could not confirm sending the queue. Check Terminal before retrying.") : error);
            } else {
                QJsonParseError error; const auto doc = QJsonDocument::fromJson(process->readAllStandardOutput(), &error); const auto receipt = doc.object();
                if (error.error != QJsonParseError::NoError || receipt.value("status") != "submitted" || receipt.value("request_id") != requestId
                    || receipt.value("name") != name || receipt.value("run_id") != run || receipt.value("conversation_id") != conversation || receipt.value("queue_id") != queue)
                    fail(tr("Invalid queue acknowledgement. Check Terminal before retrying."));
                else { *reported = true; emit queueSendFinished(request, true, receipt, {}); emit sessionWriteDone(host); }
            }
        }
        process->deleteLater();
    });
    timer->start(host.isEmpty() ? 20000 : 35000); process->start(); return request;
}

quint64 HgsClient::requestEffort(const QString &host, const QString &name, const QString &effort, const QString &run, const QString &conversation)
{
    const auto request = ++m_effortRequest;
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto payload = QJsonDocument(QJsonObject{{"request_id", requestId}, {"expected_run_id", run},
        {"expected_conversation_id", conversation}, {"effort", effort}}).toJson(QJsonDocument::Compact);
    QStringList args{"effort", name, "--json"}; if (!host.isEmpty()) args.prepend('@' + host);
    auto *process = new QProcess(this); configureProcess(process, m_hgs, args);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    const auto fail = [this, request, reported](const QString &error) {
        if (*reported) return; *reported = true; emit effortFinished(request, false, {}, error);
    };
    connect(process, &QProcess::started, process, [process, payload, fail] {
        if (process->write(payload) != payload.size()) { fail(tr("Effort transfer failed. Check Terminal before retrying.")); process->kill(); }
        process->closeWriteChannel();
    });
    connect(timer, &QTimer::timeout, process, [process, fail] {
        fail(tr("Effort change timed out. Check Terminal before retrying.")); process->kill();
    });
    connect(process, &QProcess::errorOccurred, process, [process, timer, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { timer->stop(); fail(process->errorString()); process->deleteLater(); }
    });
    connect(process, &QProcess::finished, process, [this, process, timer, fail, reported, request, requestId, name, run, conversation, effort, host](int code, QProcess::ExitStatus status) {
        timer->stop();
        if (!*reported) {
            if (code || status != QProcess::NormalExit) {
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed().left(2000);
                fail(error.isEmpty() ? tr("Could not confirm effort. Check Terminal before retrying.") : error);
            } else {
                QJsonParseError error; const auto doc = QJsonDocument::fromJson(process->readAllStandardOutput(), &error); const auto receipt = doc.object();
                if (error.error != QJsonParseError::NoError || receipt.value("status") != "confirmed" || receipt.value("request_id") != requestId
                    || receipt.value("name") != name || receipt.value("run_id") != run || receipt.value("conversation_id") != conversation || receipt.value("effort") != effort)
                    fail(tr("Invalid effort acknowledgement. Check Terminal before retrying."));
                else { *reported = true; emit effortFinished(request, true, receipt, {}); emit sessionWriteDone(host); }
            }
        }
        process->deleteLater();
    });
    timer->start(host.isEmpty() ? 20000 : 35000); process->start(); return request;
}

quint64 HgsClient::requestSettings(const QString &host, const QString &name, const QString &model, const QString &effort, const QString &run, const QString &conversation, const QString &expectedPendingId)
{
    const auto request = ++m_settingsRequest;
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject data{{"request_id", requestId}, {"expected_run_id", run}, {"expected_conversation_id", conversation}};
    if (!model.isEmpty()) data["model"] = model;
    if (!effort.isEmpty()) data["effort"] = effort;
    if (!expectedPendingId.isEmpty()) data["expected_pending_id"] = expectedPendingId;
    const auto payload = QJsonDocument(data).toJson(QJsonDocument::Compact);
    QStringList args{"settings", name, "--json"}; if (!host.isEmpty()) args.prepend('@' + host);
    auto *process = new QProcess(this); configureProcess(process, m_hgs, args);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    const auto fail = [this, request, reported](const QString &error) {
        if (*reported) return; *reported = true; emit settingsFinished(request, false, {}, error);
    };
    connect(process, &QProcess::started, process, [process, payload, fail] {
        if (process->write(payload) != payload.size()) { fail(tr("Settings transfer failed. Check Terminal before retrying.")); process->kill(); }
        process->closeWriteChannel();
    });
    connect(timer, &QTimer::timeout, process, [process, fail] {
        fail(tr("Settings change timed out. Check Terminal before retrying.")); process->kill();
    });
    connect(process, &QProcess::errorOccurred, process, [process, timer, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { timer->stop(); fail(process->errorString()); process->deleteLater(); }
    });
    connect(process, &QProcess::finished, process, [this, process, timer, fail, reported, request, requestId, name, run, conversation, model, effort, host](int code, QProcess::ExitStatus status) {
        timer->stop();
        if (!*reported) {
            if (code || status != QProcess::NormalExit) {
                const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed().left(2000);
                fail(error.isEmpty() ? tr("Could not confirm session settings. Check Terminal before retrying.") : error);
            } else {
                QJsonParseError error; const auto doc = QJsonDocument::fromJson(process->readAllStandardOutput(), &error); const auto receipt = doc.object();
                if (error.error != QJsonParseError::NoError || (receipt.value("status") != "applied" && receipt.value("status") != "scheduled") || receipt.value("request_id") != requestId
                    || receipt.value("name") != name || receipt.value("run_id") != run || receipt.value("conversation_id") != conversation
                    || (!model.isEmpty() && receipt.value("model") != model) || (!effort.isEmpty() && receipt.value("effort") != effort))
                    fail(tr("Invalid settings acknowledgement. Check Terminal before retrying."));
                else { *reported = true; emit settingsFinished(request, true, receipt, {}); emit sessionWriteDone(host); }
            }
        }
        process->deleteLater();
    });
    timer->start(host.isEmpty() ? 20000 : 35000); process->start(); return request;
}

quint64 HgsClient::requestRead(const QString &host, const QString &name, const QJsonObject &payload)
{
    const auto request = ++m_readRequest;
    QStringList args{"read", name, "--json"}; if (!host.isEmpty()) args.prepend('@' + host);
    const auto input = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    auto *process = new QProcess(this); configureProcess(process, m_hgs, args);
    auto *timer = new QTimer(process); timer->setSingleShot(true);
    auto reported = std::make_shared<bool>(false);
    const auto fail = [this, request, reported](const QString &error) {
        if (*reported) return; *reported = true; emit readFinished(request, false, {}, error);
    };
    connect(process, &QProcess::started, process, [process, input, fail] {
        if (process->write(input) != input.size()) { fail(tr("Read state transfer failed.")); process->kill(); }
        process->closeWriteChannel();
    });
    connect(timer, &QTimer::timeout, process, [process, fail] { fail(tr("Saving read state timed out.")); process->kill(); });
    connect(process, &QProcess::errorOccurred, process, [process, timer, fail](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { timer->stop(); fail(process->errorString()); process->deleteLater(); }
    });
    connect(process, &QProcess::finished, process, [this, process, timer, fail, reported, request, payload](int code, QProcess::ExitStatus status) {
        timer->stop();
        if (!*reported) {
            QJsonParseError parse; const auto result = QJsonDocument::fromJson(process->readAllStandardOutput(), &parse).object();
            const auto applied = result.value("applied").toObject();
            const bool notFound = code == 1 && result == QJsonObject{{"ok", false}, {"error", "not_found"}};
            const bool answered = code == 0 && result.value("ok") == true && applied.value("reply_id").isBool()
                && applied.value("attention_signature").isBool()
                && (result.value("read").isNull()
                    || result.value("read").toObject().value("conversation_id") == payload.value("conversation_id"));
            if (status == QProcess::NormalExit && parse.error == QJsonParseError::NoError && (notFound || answered)) {
                *reported = true; emit readFinished(request, true, result, {});
            } else {
                const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed().left(2000);
                fail(error.isEmpty() ? tr("Could not save read state on that machine.") : error);
            }
        }
        process->deleteLater();
    });
    timer->start(host.isEmpty() ? 15000 : 35000); process->start(); return request;
}
