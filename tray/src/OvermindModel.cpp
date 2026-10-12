#include "OvermindModel.h"

#include "HgsClient.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

#ifdef Q_OS_WIN
// A Windows console manages other machines and holds no Overmind replica.
constexpr bool LocalOvermind = false;
#else
constexpr bool LocalOvermind = true;
#endif

OvermindModel::Session sessionFrom(const QJsonValue &value)
{
    const auto object = value.toObject();
    return {object.value(QStringLiteral("machine")).toString(), object.value(QStringLiteral("name")).toString()};
}

QStringList strings(const QJsonValue &value)
{
    QStringList out;
    for (const auto &item : value.toArray()) out.append(item.toString());
    return out;
}

OvermindModel::Task taskFrom(const QJsonObject &object)
{
    OvermindModel::Task task;
    task.uuid = object.value(QStringLiteral("uuid")).toString();
    task.ref = object.value(QStringLiteral("ref")).toString();
    task.project = object.value(QStringLiteral("project")).toString();
    task.title = object.value(QStringLiteral("title")).toString();
    task.stage = object.value(QStringLiteral("stage")).toString();
    task.rank = object.value(QStringLiteral("rank")).toString();
    task.priority = object.value(QStringLiteral("priority")).toInt(2);
    task.labels = strings(object.value(QStringLiteral("labels")));
    task.flags = strings(object.value(QStringLiteral("flags")));
    task.markers = strings(object.value(QStringLiteral("markers")));
    task.pinned = sessionFrom(object.value(QStringLiteral("pinned")));
    task.assignee = sessionFrom(object.value(QStringLiteral("assignee")));
    task.reviewer = sessionFrom(object.value(QStringLiteral("reviewer")));
    task.queueOf = sessionFrom(object.value(QStringLiteral("queue_of")));
    task.pinnedCurrent = object.value(QStringLiteral("pinned_current")).toString();
    task.json = object;
    return task;
}

OvermindModel::Board boardFrom(const QString &project, const QJsonObject &reply)
{
    OvermindModel::Board board;
    board.project = project;
    board.loaded = true;
    board.cursor = reply.value(QStringLiteral("cursor")).toInteger(-1);
    board.workflow = reply.value(QStringLiteral("workflow")).toObject();
    for (const auto &value : board.workflow.value(QStringLiteral("stages")).toArray()) {
        const auto stage = value.toObject();
        board.stages.append({stage.value(QStringLiteral("name")).toString(), stage.value(QStringLiteral("role")).toString()});
    }
    const auto tasks = reply.value(QStringLiteral("tasks")).toArray();
    board.tasks.reserve(tasks.size());
    for (const auto &value : tasks) board.tasks.append(taskFrom(value.toObject()));
    for (const auto &task : std::as_const(board.tasks))
        if (!task.queueOf.isEmpty()) ++board.queues[task.queueOf.machine + QLatin1Char('\n') + task.queueOf.name];
    return board;
}

// What a view shows; the cursor alone moves with changes in other projects.
bool sameContent(const OvermindModel::Board &a, const OvermindModel::Board &b)
{
    if (a.loaded != b.loaded || a.error != b.error || a.workflow != b.workflow || a.tasks.size() != b.tasks.size())
        return false;
    for (qsizetype i = 0; i < a.tasks.size(); ++i)
        if (a.tasks[i].json != b.tasks[i].json) return false;
    return true;
}

OvermindModel::Decision decisionFrom(const QJsonObject &object)
{
    OvermindModel::Decision decision;
    decision.task = object.value(QStringLiteral("task")).toString();
    decision.ref = object.value(QStringLiteral("ref")).toString();
    decision.project = object.value(QStringLiteral("project")).toString();
    decision.title = object.value(QStringLiteral("title")).toString();
    decision.stage = object.value(QStringLiteral("stage")).toString();
    decision.question = object.value(QStringLiteral("question")).toString();
    decision.signature = object.value(QStringLiteral("signature")).toString();
    decision.assignee = sessionFrom(object.value(QStringLiteral("assignee")));
    return decision;
}

bool sameDecision(const OvermindModel::Decision &a, const OvermindModel::Decision &b)
{
    return a.task == b.task && a.ref == b.ref && a.project == b.project && a.title == b.title && a.stage == b.stage
        && a.question == b.question && a.signature == b.signature && a.assignee.machine == b.assignee.machine
        && a.assignee.name == b.assignee.name;
}

} // namespace

bool OvermindModel::delivers(const QString &command)
{
    static const QStringList commands{QStringLiteral("send"), QStringLiteral("review-send"), QStringLiteral("rework-send"),
                                      QStringLiteral("answer"), QStringLiteral("resolve-delivery")};
    return commands.contains(command);
}

QString OvermindModel::Board::role(const QString &stage) const
{
    for (const auto &candidate : stages)
        if (candidate.name == stage) return candidate.role;
    return {};
}

OvermindModel::OvermindModel(HgsClient *client, QObject *parent)
    : QObject(parent), m_client(client), m_enabled(LocalOvermind)
{
    connect(m_client, &HgsClient::overmindReady, this, &OvermindModel::replied);
    connect(m_client, &HgsClient::overmindFailed, this, &OvermindModel::failed);
    connect(&m_timer, &QTimer::timeout, this, &OvermindModel::refresh);
    if (!m_enabled) return;
    m_timer.start(m_idleMs);
    // The decisions matter before any board is shown (Your Call notifications).
    QTimer::singleShot(0, this, &OvermindModel::refresh);
}

void OvermindModel::setEnabled(bool enabled)
{
    if (enabled == m_enabled) return;
    m_enabled = enabled;
    reset();
}

void OvermindModel::setHost(const QString &host)
{
    const bool enabled = !host.isEmpty() || (LocalOvermind && m_enabled);
    if (host == m_host && enabled == m_enabled) return;
    m_host = host;
    m_enabled = enabled;
    reset();
}

// Forgets everything read so far and, when enabled, reads it again.
void OvermindModel::reset()
{
    ++m_generation;
    // Reads in flight answer for the previous host: forget them. Writes still
    // report to their callers.
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->kind == Kind::Write || it->kind == Kind::Read) ++it;
        else it = m_pending.erase(it);
    }
    m_revInFlight = m_revAgain = m_decisionsInFlight = m_decisionsLoaded = m_statusInFlight = false;
    m_boardsInFlight.clear();
    m_boardsFailedAt.clear();
    m_decisionsFailedAt = m_statusFailedAt = Never;
    m_cursor = m_decisionsCursor = -1;
    m_decisions.clear();
    m_decisionsError.clear();
    const bool hadMachine = !m_machine.isEmpty();
    m_machine.clear();
    setError({});
    for (const auto &project : std::as_const(m_watched)) {
        Board empty; empty.project = project;
        m_boards[project] = empty;
        emit boardChanged(project);
    }
    emit decisionsChanged();
    if (hadMachine) emit machineChanged();
    if (!m_enabled) {
        m_timer.stop();
        return;
    }
    m_timer.start(m_active ? m_activeMs : m_idleMs);
    for (const auto &project : std::as_const(m_watched)) loadBoard(project);
    refresh();
}

void OvermindModel::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    if (!m_enabled) return;
    m_timer.start(m_active ? m_activeMs : m_idleMs);
    if (m_active) refresh();
}

void OvermindModel::setIntervals(int activeMs, int idleMs)
{
    m_activeMs = activeMs;
    m_idleMs = idleMs;
    if (m_enabled) m_timer.start(m_active ? m_activeMs : m_idleMs);
}

void OvermindModel::watchProject(const QString &project)
{
    if (m_watched.contains(project)) {
        if (!board(project).error.isEmpty()) {
            m_boardsFailedAt.remove(project);
            loadBoard(project);
        }
        return;
    }
    m_watched.append(project);
    Board empty; empty.project = project;
    m_boards.insert(project, empty);
    loadBoard(project);
}

void OvermindModel::unwatchProject(const QString &project)
{
    m_watched.removeAll(project);
    m_boards.remove(project);
    m_boardsFailedAt.remove(project);
}

const OvermindModel::Board &OvermindModel::board(const QString &project) const
{
    static const Board empty;
    const auto it = m_boards.constFind(project);
    return it == m_boards.constEnd() ? empty : *it;
}

int OvermindModel::queueCount(const QString &machine, const QString &session, const QString &project) const
{
    const QString &owner = machine.isEmpty() ? m_machine : machine;
    if (owner.isEmpty() || session.isEmpty()) return 0;
    const QString key = owner + QLatin1Char('\n') + session;
    if (!project.isEmpty()) return board(project).queues.value(key);
    int count = 0;
    for (const auto &board : m_boards) count += board.queues.value(key);
    return count;
}

QList<OvermindModel::Decision> OvermindModel::decisionsFor(const QString &machine, const QString &session, const QString &project) const
{
    const QString &owner = machine.isEmpty() ? m_machine : machine;
    QList<Decision> out;
    if (owner.isEmpty() || session.isEmpty()) return out;
    for (const auto &decision : m_decisions)
        if (decision.assignee.name == session && decision.assignee.machine == owner && (project.isEmpty() || decision.project == project))
            out.append(decision);
    return out;
}

quint64 OvermindModel::write(const QString &command, QJsonObject request, Callback callback, QObject *context)
{
    // A repeated request_id replays the stored result instead of writing twice.
    if (request.value(QStringLiteral("request_id")).toString().isEmpty())
        request.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    Pending pending;
    pending.kind = Kind::Write;
    pending.name = command;
    pending.callback = std::move(callback);
    pending.context = context;
    pending.guarded = context != nullptr;
    pending.requestId = request.value(QStringLiteral("request_id")).toString();
    const bool remote = !m_host.isEmpty();
    const int timeout = delivers(command) ? (remote ? m_timeouts.deliveryRemote : m_timeouts.deliveryLocal)
                                          : (remote ? m_timeouts.remote : m_timeouts.local);
    return track(std::move(pending), command, request, timeout);
}

quint64 OvermindModel::read(const QString &command, const QJsonObject &request, Callback callback, QObject *context)
{
    Pending pending;
    pending.kind = Kind::Read;
    pending.name = command;
    pending.callback = std::move(callback);
    pending.context = context;
    pending.guarded = context != nullptr;
    return track(std::move(pending), command, request);
}

void OvermindModel::refresh()
{
    if (!m_enabled) return;
    if (m_revInFlight) {
        m_revAgain = true;
        return;
    }
    m_revInFlight = true;
    track({Kind::Rev}, QStringLiteral("rev"), {});
}

quint64 OvermindModel::send(const QString &command, const QJsonObject &request, int timeoutMs)
{
    if (timeoutMs <= 0) timeoutMs = m_host.isEmpty() ? m_timeouts.local : m_timeouts.remote;
    return m_client->requestOvermind(m_host, command.split(QLatin1Char(' '), Qt::SkipEmptyParts), request, timeoutMs);
}

quint64 OvermindModel::track(Pending pending, const QString &command, const QJsonObject &request, int timeoutMs)
{
    if (!m_enabled) {
        failLater(std::move(pending));
        return 0;
    }
    pending.generation = m_generation;
    const quint64 id = send(command, request, timeoutMs);
    m_pending.insert(id, std::move(pending));
    return id;
}

// A write or read while disabled fails like any other, after write() returned.
void OvermindModel::failLater(Pending pending)
{
    QMetaObject::invokeMethod(this, [this, pending = std::move(pending)]() {
        Result result;
        result.refusal = {{QStringLiteral("code"), QStringLiteral("unavailable")},
                          {QStringLiteral("message"), tr("This computer has no task board. Select a session on a machine with Overmind.")}};
        result.requestId = pending.requestId;
        if (pending.kind == Kind::Write) emit writeFailed(pending.name, result.refusal, result.requestId);
        finish(pending, result);
    }, Qt::QueuedConnection);
}

void OvermindModel::finish(const Pending &pending, const Result &result)
{
    if (pending.callback && (!pending.guarded || pending.context)) pending.callback(result);
}

void OvermindModel::reloadStale()
{
    for (const auto &project : std::as_const(m_watched))
        if (board(project).cursor != m_cursor && m_boardsFailedAt.value(project, Never) != m_cursor) loadBoard(project);
    if (m_decisionsCursor != m_cursor && m_decisionsFailedAt != m_cursor) loadDecisions();
    if (m_machine.isEmpty() && m_statusFailedAt != m_cursor) loadStatus();
}

void OvermindModel::loadBoard(const QString &project)
{
    if (!m_enabled || m_boardsInFlight.contains(project)) return;
    m_boardsInFlight.append(project);
    Pending pending;
    pending.kind = Kind::Board;
    pending.name = project;
    track(std::move(pending), QStringLiteral("board"), {{QStringLiteral("project"), project}});
}

void OvermindModel::loadDecisions()
{
    if (!m_enabled || m_decisionsInFlight) return;
    m_decisionsInFlight = true;
    track({Kind::Decisions}, QStringLiteral("decisions"), {});
}

void OvermindModel::loadStatus()
{
    if (!m_enabled || m_statusInFlight) return;
    m_statusInFlight = true;
    track({Kind::Status}, QStringLiteral("status"), {});
}

void OvermindModel::setError(const QString &error)
{
    if (error == m_error) return;
    m_error = error;
    emit errorChanged();
}

void OvermindModel::replied(quint64 request, const QString &, const QJsonObject &reply)
{
    const auto it = m_pending.find(request);
    if (it == m_pending.end()) return;
    const Pending pending = std::move(*it);
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::Rev:
        m_revInFlight = false;
        setError({});
        m_cursor = reply.value(QStringLiteral("cursor")).toInteger(-1);
        reloadStale();
        if (std::exchange(m_revAgain, false)) refresh();
        return;
    case Kind::Board: {
        m_boardsInFlight.removeOne(pending.name);
        if (!m_watched.contains(pending.name)) return;
        m_boardsFailedAt.remove(pending.name);
        auto fresh = boardFrom(pending.name, reply);
        const qint64 cursor = fresh.cursor;
        auto &current = m_boards[pending.name];
        const bool changed = !sameContent(current, fresh);
        current = std::move(fresh);
        if (changed) emit boardChanged(pending.name);
        // The store moved on while this board was read.
        if (cursor >= 0 && cursor < m_cursor) loadBoard(pending.name);
        return;
    }
    case Kind::Decisions: {
        m_decisionsInFlight = false;
        m_decisionsFailedAt = Never;
        QList<Decision> decisions;
        for (const auto &value : reply.value(QStringLiteral("decisions")).toArray()) decisions.append(decisionFrom(value.toObject()));
        const bool changed = !m_decisionsLoaded || !m_decisionsError.isEmpty() || m_decisions.size() != decisions.size()
            || !std::equal(m_decisions.cbegin(), m_decisions.cend(), decisions.cbegin(), sameDecision);
        m_decisions = std::move(decisions);
        m_decisionsError.clear();
        m_decisionsLoaded = true;
        m_decisionsCursor = reply.value(QStringLiteral("cursor")).toInteger(-1);
        if (changed) emit decisionsChanged();
        if (m_decisionsCursor >= 0 && m_decisionsCursor < m_cursor) loadDecisions();
        return;
    }
    case Kind::Status: {
        m_statusInFlight = false;
        m_statusFailedAt = Never;
        const auto machine = reply.value(QStringLiteral("machine")).toString();
        if (machine == m_machine) return;
        m_machine = machine;
        emit machineChanged();
        return;
    }
    case Kind::Write: {
        Result result;
        result.ok = true;
        result.reply = reply;
        result.requestId = pending.requestId;
        if (pending.generation == m_generation) refresh();
        finish(pending, result);   // last: the callback may destroy this model
        return;
    }
    case Kind::Read: {
        Result result;
        result.ok = true;
        result.reply = reply;
        finish(pending, result);
        return;
    }
    }
}

void OvermindModel::failed(quint64 request, const QString &, const QJsonObject &refusal, const QString &detail)
{
    const auto it = m_pending.find(request);
    if (it == m_pending.end()) return;
    const Pending pending = std::move(*it);
    m_pending.erase(it);
    switch (pending.kind) {
    case Kind::Rev:
        m_revInFlight = false;
        setError(detail);
        if (std::exchange(m_revAgain, false)) refresh();
        return;
    case Kind::Board: {
        m_boardsInFlight.removeOne(pending.name);
        if (!m_watched.contains(pending.name)) return;
        m_boardsFailedAt.insert(pending.name, m_cursor);
        auto &board = m_boards[pending.name];
        if (board.error == detail && board.refusal == refusal) return;
        board.error = detail;
        board.refusal = refusal;
        emit boardChanged(pending.name);
        return;
    }
    case Kind::Decisions:
        m_decisionsInFlight = false;
        m_decisionsFailedAt = m_cursor;
        if (detail == m_decisionsError) return;
        m_decisionsError = detail;
        emit decisionsChanged();
        return;
    case Kind::Status:
        m_statusInFlight = false;
        m_statusFailedAt = m_cursor;
        return;
    case Kind::Write:
    case Kind::Read: {
        Result result;
        result.refusal = refusal;
        result.requestId = pending.requestId;
        if (pending.kind == Kind::Write && result.uncertain())
            result.refusal.insert(QStringLiteral("message"), tr("The task board did not answer in time. The change may have been made."));
        if (result.refusal.value(QStringLiteral("message")).toString().isEmpty())
            result.refusal.insert(QStringLiteral("message"), detail);
        if (pending.kind == Kind::Write) emit writeFailed(pending.name, result.refusal, result.requestId);
        finish(pending, result);   // last: the callback may destroy this model
        return;
    }
    }
}
