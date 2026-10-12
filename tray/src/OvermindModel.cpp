#include "OvermindModel.h"

#include "HgsClient.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

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

} // namespace

QString OvermindModel::Board::role(const QString &stage) const
{
    for (const auto &candidate : stages)
        if (candidate.name == stage) return candidate.role;
    return {};
}

OvermindModel::OvermindModel(HgsClient *client, QObject *parent)
    : QObject(parent), m_client(client)
{
    connect(m_client, &HgsClient::overmindReady, this, &OvermindModel::replied);
    connect(m_client, &HgsClient::overmindFailed, this, &OvermindModel::failed);
    connect(&m_timer, &QTimer::timeout, this, &OvermindModel::refresh);
    m_timer.start(m_idleMs);
    // The decisions matter before any board is shown (Your Call notifications).
    QTimer::singleShot(0, this, &OvermindModel::refresh);
}

void OvermindModel::setHost(const QString &host)
{
    if (host == m_host) return;
    m_host = host;
    ++m_generation;
    // Reads in flight answer for the previous host: forget them. Writes still
    // report to their callers.
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (it->kind == Kind::Write || it->kind == Kind::Read) ++it;
        else it = m_pending.erase(it);
    }
    m_revInFlight = m_revAgain = m_decisionsInFlight = m_decisionsLoaded = false;
    m_boardsInFlight.clear();
    m_cursor = m_decisionsCursor = -1;
    m_decisions.clear();
    setError({});
    for (const auto &project : std::as_const(m_watched)) {
        Board empty; empty.project = project;
        m_boards[project] = empty;
        emit boardChanged(project);
    }
    emit decisionsChanged();
    for (const auto &project : std::as_const(m_watched)) loadBoard(project);
    refresh();
}

void OvermindModel::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    m_timer.start(m_active ? m_activeMs : m_idleMs);
    if (m_active) refresh();
}

void OvermindModel::setIntervals(int activeMs, int idleMs)
{
    m_activeMs = activeMs;
    m_idleMs = idleMs;
    m_timer.start(m_active ? m_activeMs : m_idleMs);
}

void OvermindModel::watchProject(const QString &project)
{
    if (m_watched.contains(project)) return;
    m_watched.append(project);
    Board empty; empty.project = project;
    m_boards.insert(project, empty);
    loadBoard(project);
}

void OvermindModel::unwatchProject(const QString &project)
{
    m_watched.removeAll(project);
    m_boards.remove(project);
}

const OvermindModel::Board &OvermindModel::board(const QString &project) const
{
    static const Board empty;
    const auto it = m_boards.constFind(project);
    return it == m_boards.constEnd() ? empty : *it;
}

int OvermindModel::queueCount(const QString &machine, const QString &session) const
{
    if (session.isEmpty()) return 0;
    int count = 0;
    for (const auto &board : m_boards)
        for (const auto &task : board.tasks)
            if (task.queueOf.name == session && (machine.isEmpty() || task.queueOf.machine == machine)) ++count;
    return count;
}

quint64 OvermindModel::write(const QString &command, QJsonObject request, Callback callback)
{
    // A repeated request_id replays the stored result instead of writing twice.
    if (request.value(QStringLiteral("request_id")).toString().isEmpty())
        request.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    const quint64 id = send(command, request);
    m_pending.insert(id, {Kind::Write, command, std::move(callback), m_generation});
    return id;
}

quint64 OvermindModel::read(const QString &command, const QJsonObject &request, Callback callback)
{
    const quint64 id = send(command, request);
    m_pending.insert(id, {Kind::Read, command, std::move(callback), m_generation});
    return id;
}

void OvermindModel::refresh()
{
    if (m_revInFlight) {
        m_revAgain = true;
        return;
    }
    m_revInFlight = true;
    m_pending.insert(send(QStringLiteral("rev"), {}), {Kind::Rev, {}, {}, m_generation});
}

quint64 OvermindModel::send(const QString &command, const QJsonObject &request)
{
    return m_client->requestOvermind(m_host, command.split(QLatin1Char(' '), Qt::SkipEmptyParts), request);
}

void OvermindModel::reloadStale()
{
    for (const auto &project : std::as_const(m_watched))
        if (board(project).cursor != m_cursor) loadBoard(project);
    if (m_decisionsCursor != m_cursor) loadDecisions();
}

void OvermindModel::loadBoard(const QString &project)
{
    if (m_boardsInFlight.contains(project)) return;
    m_boardsInFlight.append(project);
    m_pending.insert(send(QStringLiteral("board"), {{QStringLiteral("project"), project}}), {Kind::Board, project, {}, m_generation});
}

void OvermindModel::loadDecisions()
{
    if (m_decisionsInFlight) return;
    m_decisionsInFlight = true;
    m_pending.insert(send(QStringLiteral("decisions"), {}), {Kind::Decisions, {}, {}, m_generation});
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
        QList<Decision> decisions;
        for (const auto &value : reply.value(QStringLiteral("decisions")).toArray()) decisions.append(decisionFrom(value.toObject()));
        const auto previous = std::exchange(m_decisions, decisions);
        const bool changed = !m_decisionsLoaded || previous.size() != decisions.size()
            || !std::equal(previous.cbegin(), previous.cend(), decisions.cbegin(), [](const Decision &a, const Decision &b) {
                   return a.task == b.task && a.ref == b.ref && a.project == b.project && a.title == b.title && a.stage == b.stage
                       && a.question == b.question && a.signature == b.signature && a.assignee.machine == b.assignee.machine
                       && a.assignee.name == b.assignee.name;
               });
        m_decisionsLoaded = true;
        m_decisionsCursor = reply.value(QStringLiteral("cursor")).toInteger(-1);
        if (changed) emit decisionsChanged();
        if (m_decisionsCursor >= 0 && m_decisionsCursor < m_cursor) loadDecisions();
        return;
    }
    case Kind::Write:
        if (pending.callback) pending.callback(true, reply, {});
        if (pending.generation == m_generation) refresh();
        return;
    case Kind::Read:
        if (pending.callback) pending.callback(true, reply, {});
        return;
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
        auto &board = m_boards[pending.name];
        if (board.error == detail && board.refusal == refusal) return;
        board.error = detail;
        board.refusal = refusal;
        emit boardChanged(pending.name);
        return;
    }
    case Kind::Decisions:
        m_decisionsInFlight = false;
        return;
    case Kind::Write:
    case Kind::Read: {
        auto reason = refusal;
        if (reason.value(QStringLiteral("message")).toString().isEmpty()) reason.insert(QStringLiteral("message"), detail);
        if (pending.callback) pending.callback(false, {}, reason);
        if (pending.kind == Kind::Write) emit writeFailed(pending.name, reason);
        return;
    }
    }
}
