#include "AttentionTracker.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>

namespace {
// How long and how many signatures of decisions that left the list are kept.
constexpr qint64 RecentSeconds = 24 * 60 * 60;
constexpr int RecentLimit = 200;
QString encode(const QJsonObject &target)
{
    return QString::fromLatin1(QJsonDocument(target).toJson(QJsonDocument::Compact)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QString noticeId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString identity(const SessionInfo &s)
{
    return !s.runId.isEmpty() ? "run:" + s.runId : "name:" + s.name + ':' + QString::number(s.created);
}
QString concise(QString value, int limit)
{
    value = value.simplified();
    for (qsizetype i = 0; i < value.size(); ++i)
        if (value[i].category() == QChar::Other_Control) value[i] = ' ';
    return value.size() > limit ? value.left(limit - 1) + QChar(0x2026) : value;
}
}

AttentionTracker::AttentionTracker(QJsonObject saved, QJsonObject decisions) : m_hosts(std::move(saved))
{
    // The first builds kept only the tasks.
    if (!decisions.contains("tasks")) {
        m_decisions = decisions;
        return;
    }
    m_decisions = decisions.value("tasks").toObject();
    m_summary = decisions.value("summary").toObject();
    m_recent = decisions.value("recent").toObject();
}

AttentionChanges AttentionTracker::observe(const QString &host, const BoxState &box, qint64 now)
{
    const auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    return observe(host, box, now, application && application->applicationState() == Qt::ApplicationActive);
}

AttentionChanges AttentionTracker::observe(const QString &host, const BoxState &box, qint64 now, bool applicationFocused)
{
    AttentionChanges changes;
    if (!box.ok) return changes;
    const auto previous = m_hosts.value(host).toObject();
    QJsonObject current;
    const auto notice = [&](const SessionInfo &s, const QString &key, const QString &phase,
                            const QString &request, const QString &reason, bool reply = false, bool silent = false) {
        const auto old = previous.value(key).toObject();
        if (silent) {
            // Withdraw the banner but remember the request without a token: if an
            // acknowledgement lapses (an older host, a recovery-only identity) or a
            // schedule ends, the same request must not be announced again.
            current[key] = QJsonObject{{"phase", phase}, {"request", request}};
            return;
        }
        const auto retry = old.value("retry_at").toInteger();
        if (old.value("phase").toString() == phase && old.value("request").toString() == request
            && (!retry || retry > now)) {
            current[key] = old;
            return;
        }
        QJsonObject target{{"host", host}, {"name", s.name}, {"run", s.runId},
            {"created", s.created}, {"notice", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
        if (reply) { target["reply"] = s.replyId; target["conversation"] = s.conversationId; target["agent"] = s.cmd; }
        const QString token = QString::fromLatin1(QJsonDocument(target).toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        current[key] = QJsonObject{{"phase", phase}, {"request", request}, {"token", token}};
        const QString body = concise(s.name, 120) + " / " + concise(host.isEmpty() ? box.host : host, 40)
            + '\n' + reason;
        // Consume foreground edges without changing unread/action state. They
        // must not become delayed banners after focus leaves or the GUI restarts.
        if (!applicationFocused)
            changes.raised.append({token, reply ? QObject::tr("Zerus — New reply") : QObject::tr("Zerus — Needs attention"), body});
    };
    for (const auto &s : box.sessions) {
        // Acknowledged here or on another device: the existing clear path withdraws it.
        if (s.needsAction()) {
            const QString reason = s.phase == "approval" ? QObject::tr("Approval needed")
                : s.phase == "input" ? QObject::tr("Input needed")
                : !s.recovery.value("reason").toString().isEmpty() ? concise(s.recovery.value("reason").toString(), 240)
                : !s.activityDetail.isEmpty() ? concise(s.activityDetail, 240) : QObject::tr("Agent needs attention");
            notice(s, identity(s), s.phase, s.attentionId, reason, false, s.attentionAcknowledged);
        }
        // Read state comes from FleetState, not the raw CLI snapshot. Replies
        // belong to conversations so a rename/resume does not repeat a banner.
        if (s.state != "archived" && s.unreadReply && !s.replyId.isEmpty() && !s.conversationId.isEmpty()) {
            const auto key = "reply:" + s.cmd + ':' + s.conversationId;
            // A main Stop can be a progress reply while background work is
            // still running. Keep it unread, but wait for confirmed idleness
            // before posting a banner. Retain delivered identities below so
            // a working/idle cycle cannot notify again for the same reply.
            const bool idle = s.activity == "idle" && s.phase == "idle" && s.backgroundProcesses <= 0 && s.subagentActiveCount == 0 && !s.needsAction();
            // Hosts do not report routine ticks of a recurring scheduled job as
            // replies; consume any that arrive. The reply after the schedule
            // ends, or once its count is known, notifies as usual.
            if (idle && s.replyScheduled && s.recurringSchedules > 0)
                notice(s, key, "reply", s.replyId, {}, true, true);
            else if (idle && (!s.replyScheduled || s.recurringSchedules == 0))
                notice(s, key, "reply", s.replyId, QObject::tr("Agent has a new reply"), true);
            else if (previous.value(key).toObject().value("request") == s.replyId) current[key] = previous.value(key);
        }
        if (s.state != "running" || s.processState == "exited" || s.activity == "unknown"
            || (s.conversationState == "ended" && s.activity != "idle") || s.subagentSource == "hook_profiles") continue;
        for (auto it = s.subagents.begin(); it != s.subagents.end(); ++it) {
            const auto child = it.value().toObject();
            const auto phase = child.value("display_state").toString(child.value("state").toString());
            if (phase != "approval" && phase != "input" && phase != "attention") continue;
            const auto request = child.value("attention_id").toString(child.value("question_id").toString());
            const auto label = child.value("label").toString(child.value("name").toString(it.key()));
            notice(s, "child:" + identity(s) + ':' + it.key(), phase, request,
                QObject::tr("Subagent %1: %2").arg(concise(label, 100), phase == "approval" ? QObject::tr("Approval needed") : QObject::tr("Input needed")),
                false, s.attentionAcknowledged);
        }
    }
    for (auto it = previous.begin(); it != previous.end(); ++it) {
        const QString token = it.value().toObject().value("token").toString();
        if (!token.isEmpty() && current.value(it.key()).toObject().value("token").toString() != token)
            changes.cleared << token;
    }
    m_hosts[host] = current;
    return changes;
}

void AttentionTracker::deliveryFailed(const QString &token, qint64 now)
{
    for (auto entry = m_decisions.begin(); entry != m_decisions.end(); ++entry) {
        auto value = entry.value().toObject();
        if (value.value("token") != token) continue;
        value["retry_at"] = now + 60;
        m_decisions[entry.key()] = value;
        return;
    }
    if (!token.isEmpty() && m_summary.value("token").toString() == token) {
        m_summary["retry_at"] = now + 60;
        return;
    }
    for (auto host = m_hosts.begin(); host != m_hosts.end(); ++host) {
        auto entries = host.value().toObject();
        for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
            auto value = entry.value().toObject();
            if (value.value("token") != token) continue;
            value["retry_at"] = now + 60;
            entries[entry.key()] = value;
            m_hosts[host.key()] = entries;
            return;
        }
    }
}

QJsonObject AttentionTracker::target(const QString &token)
{
    if (token.size() > 8192) return {};
    const auto result = QJsonDocument::fromJson(QByteArray::fromBase64(token.toLatin1(), QByteArray::Base64UrlEncoding)).object();
    if (!result.value("host").isString() || result.value("name").toString().isEmpty()
        || result.value("notice").toString().isEmpty()) return {};
    return result;
}

AttentionChanges AttentionTracker::observeDecisions(const OvermindModel &model, const ProjectNames &names)
{
    const auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    return observeDecisions(model, names, QDateTime::currentSecsSinceEpoch(),
                            application && application->applicationState() == Qt::ApplicationActive);
}

AttentionChanges AttentionTracker::observeDecisions(const OvermindModel &model, const ProjectNames &names, qint64 now,
                                                    bool applicationFocused)
{
    // Not loaded yet, a failed read, or the reset after a host change.
    const bool read = model.decisionsLoaded() && model.decisionsError().isEmpty();
    return observeDecisions(read, model.decisions(), now, applicationFocused, names);
}

AttentionChanges AttentionTracker::observeDecisions(bool read, const QList<OvermindModel::Decision> &decisions, qint64 now,
                                                    bool applicationFocused, const ProjectNames &names)
{
    AttentionChanges changes;
    if (!read) return changes;
    for (auto it = m_recent.begin(); it != m_recent.end();) {
        if (it.value().toInteger() < now - RecentSeconds) it = m_recent.erase(it);
        else ++it;
    }
    QJsonObject current;
    QList<AttentionNotice> fresh;
    QString firstProject;
    for (const auto &d : decisions) {
        if (d.task.isEmpty() || current.contains(d.task)) continue;
        const auto old = m_decisions.value(d.task).toObject();
        const auto retry = old.value("retry_at").toInteger();
        if (old.value("signature").toString() == d.signature && (!retry || retry > now)) {
            current[d.task] = old;
            continue;
        }
        // Back unchanged after it left the list (a stale or capped read): it
        // was announced already.
        if (old.isEmpty() && !d.signature.isEmpty() && m_recent.contains(d.signature)) {
            current[d.task] = QJsonObject{{"signature", d.signature}};
            m_recent.remove(d.signature);
            continue;
        }
        const QString token = encode({{"decision", d.task}, {"project", d.project}, {"machine", d.assignee.machine},
                                      {"name", d.assignee.name}, {"notice", noticeId()}});
        current[d.task] = QJsonObject{{"signature", d.signature}, {"token", token}};
        // As for sessions: a decision that arrives while Zerus has focus is
        // consumed, never shown later; the Tasks tab and the badges show it.
        if (applicationFocused) continue;
        QString where = concise(names.value(d.project), 60);
        if (!d.assignee.name.isEmpty()) where += (where.isEmpty() ? QString() : QStringLiteral(" / ")) + concise(d.assignee.name, 120);
        QString body = concise(d.ref + ": " + d.title, 160) + '\n'
            + (d.question.isEmpty() ? QObject::tr("A task waits for your call.") : concise(d.question, 240));
        if (!where.isEmpty()) body += '\n' + where;
        if (fresh.isEmpty()) firstProject = d.project;
        fresh.append({token, QObject::tr("Zerus — Your Call"), body, QObject::tr("Open task")});
    }
    for (auto it = m_decisions.begin(); it != m_decisions.end(); ++it) {
        const auto signature = it.value().toObject().value("signature").toString();
        if (!current.contains(it.key()) && !signature.isEmpty()) m_recent[signature] = now;
    }
    while (m_recent.size() > RecentLimit) {
        auto oldest = m_recent.begin();
        for (auto it = m_recent.begin(); it != m_recent.end(); ++it)
            if (it.value().toInteger() < oldest.value().toInteger()) oldest = it;
        m_recent.erase(oldest);
    }

    // A burst (the first start, say) is one summary notice; its decisions
    // keep their own tokens so that they are never announced again.
    QJsonObject summary;
    if (fresh.size() > DecisionBurst) {
        QJsonArray tokens;
        QStringList lines;
        for (const auto &notice : std::as_const(fresh)) {
            tokens.append(notice.token);
            if (lines.size() < DecisionBurst) lines << notice.body.section('\n', 0, 0);
        }
        const QString token = encode({{"decisions", int(fresh.size())}, {"project", firstProject}, {"notice", noticeId()}});
        summary = QJsonObject{{"token", token}, {"tokens", tokens}, {"project", firstProject}};
        changes.raised.append({token, QObject::tr("Zerus — Your Call"),
            QObject::tr("%1 tasks wait for your call.").arg(fresh.size()) + '\n' + lines.join('\n') + '\n' + QChar(0x2026),
            QObject::tr("Open tasks")});
    } else {
        changes.raised = fresh;
        // An earlier summary stays while one of its decisions still waits as announced.
        QSet<QString> waiting;
        for (auto it = current.begin(); it != current.end(); ++it) waiting.insert(it.value().toObject().value("token").toString());
        QJsonArray tokens;
        for (const auto &token : m_summary.value("tokens").toArray())
            if (waiting.contains(token.toString())) tokens.append(token);
        if (!tokens.isEmpty()) {
            summary = m_summary;
            summary["tokens"] = tokens;
            const auto retry = summary.value("retry_at").toInteger();
            if (retry && retry <= now) {
                summary.remove("retry_at");
                if (!applicationFocused) {
                    const QString token = encode({{"decisions", int(tokens.size())}, {"project", summary.value("project")}, {"notice", noticeId()}});
                    summary["token"] = token;
                    changes.raised.append({token, QObject::tr("Zerus — Your Call"),
                        QObject::tr("%1 tasks wait for your call.").arg(tokens.size()), QObject::tr("Open tasks")});
                }
            }
        }
    }

    for (auto it = m_decisions.begin(); it != m_decisions.end(); ++it) {
        const QString token = it.value().toObject().value("token").toString();
        if (!token.isEmpty() && current.value(it.key()).toObject().value("token").toString() != token)
            changes.cleared << token;
    }
    const QString previous = m_summary.value("token").toString();
    if (!previous.isEmpty() && summary.value("token").toString() != previous) changes.cleared << previous;
    m_decisions = current;
    m_summary = summary;
    return changes;
}

QJsonObject AttentionTracker::decisionState() const
{
    QJsonObject state{{"tasks", m_decisions}};
    if (!m_summary.isEmpty()) state["summary"] = m_summary;
    if (!m_recent.isEmpty()) state["recent"] = m_recent;
    return state;
}

QJsonObject AttentionTracker::decision(const QString &token)
{
    if (token.size() > 8192) return {};
    const auto result = QJsonDocument::fromJson(QByteArray::fromBase64(token.toLatin1(), QByteArray::Base64UrlEncoding)).object();
    if (!result.value("project").isString() || result.value("notice").toString().isEmpty()) return {};
    if (result.value("decisions").toInt() > 0) return QJsonObject{{"task", QString()}, {"project", result.value("project")},
        {"machine", QString()}, {"name", QString()}};
    if (result.value("decision").toString().isEmpty()) return {};
    return QJsonObject{{"task", result.value("decision")}, {"project", result.value("project")},
        {"machine", result.value("machine").toString()}, {"name", result.value("name").toString()}};
}

bool AttentionTracker::matches(const QJsonObject &target, const SessionInfo &s)
{
    if (target.isEmpty() || s.state == "archived") return false;
    if (!target.value("reply").toString().isEmpty())
        return target.value("conversation").toString() == s.conversationId && target.value("agent").toString() == s.cmd;
    const auto run = target.value("run").toString();
    return run.isEmpty() ? target.value("name").toString() == s.name && target.value("created").toInteger() == s.created
                         : run == s.runId;
}
