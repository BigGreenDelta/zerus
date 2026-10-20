#include "AttentionTracker.h"
#include "SessionPresentation.h"
#include "SessionNotifications.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>

#include <optional>

namespace {
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
// Question IDs already seen per conversation, including answered ones: a
// transient empty listing, a resume or a connector restart must not announce
// the same question again.
constexpr qsizetype SeenQuestions = 64;
bool suppressedMain(const SessionInfo &s, const QJsonObject &baseline) {
    const auto main = baseline.value("main_attention").toObject();
    return !main.isEmpty() && main.value("run_id").toString() == s.runId
        && SessionNotifications::matches(main, s.phase, s.attentionId, s.attentionStarted);
}
bool suppressedChild(const QJsonObject &baseline, const QString &id, const QJsonObject &child) {
    const auto saved = baseline.value("child_attention").toObject().value(id).toObject();
    return !saved.isEmpty() && SessionNotifications::matches(saved, SessionNotifications::childPhase(child),
        SessionNotifications::childRequest(child), SessionNotifications::childStarted(child));
}
QString notificationRequest(const QString &request, const QJsonValue &started) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{request,
        request.isEmpty() ? SessionNotifications::positive(started) : QJsonValue()}).toJson(QJsonDocument::Compact));
}
}

AttentionChanges AttentionTracker::observe(const QString &host, const BoxState &box, qint64 now)
{
    const auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    return observe(host, box, now, application && application->applicationState() == Qt::ApplicationActive);
}

AttentionChanges AttentionTracker::observe(const QString &host, const BoxState &box, qint64 now, bool applicationFocused)
{
    AttentionChanges changes;
    const auto previous = m_hosts.value(host).toObject();
    if (!box.ok) {
        // A confirmed host mute can withdraw an offline peer's existing banner.
        // Retain its observed event/seen IDs: an offline listing cannot resolve
        // requests, discover events or turn unmute into a delayed notification.
        QSet<QString> mutedRuns, mutedConversations;
        for (const auto &s : box.sessions) if (s.muted && s.state != "archived") {
            mutedRuns.insert(identity(s));
            if (!s.conversationId.isEmpty()) mutedConversations.insert(s.cmd + ':' + s.conversationId);
        }
        auto retained = previous;
        for (auto it = retained.begin(); it != retained.end(); ++it) {
            auto entry = it.value().toObject();
            const auto token = entry.value("token").toString();
            if (token.isEmpty()) continue;
            const auto destination = target(token);
            if (destination.isEmpty() || destination.value("host").toString() != host) continue;
            const bool conversation = !destination.value("reply").toString().isEmpty() || !destination.value("question").toString().isEmpty();
            const auto run = destination.value("run").toString();
            const auto key = run.isEmpty() ? "name:" + destination.value("name").toString() + ':' + QString::number(destination.value("created").toInteger()) : "run:" + run;
            const bool muted = conversation ? mutedConversations.contains(destination.value("agent").toString() + ':' + destination.value("conversation").toString())
                : mutedRuns.contains(key);
            if (!muted) continue;
            changes.cleared << token; entry.remove("token"); entry.remove("retry_at"); it.value() = entry;
        }
        if (retained != previous) m_hosts[host] = retained;
        return changes;
    }
    // A downgraded/offline host loses write authority, not its last validated
    // suppression baseline. Validate once per row, independently of capability.
    QHash<const SessionInfo *, QJsonObject> baselines;
    for (const auto &s : box.sessions)
        if (SessionNotifications::valid(s.notificationPreferences, s.conversationId))
            baselines.insert(&s, s.notificationPreferences.value("suppressed").toObject());
    QJsonObject current;
    const auto notice = [&](const SessionInfo &s, const QString &key, const QString &phase,
                            const QString &request, const QString &reason, bool reply = false, bool silent = false,
                            std::optional<double> started = std::nullopt) {
        const auto old = previous.value(key).toObject();
        QJsonObject entry{{"phase", phase}, {"request", request}};
        if (started) entry["started"] = *started;
        // An identity-less main request is distinguished by its start time.
        // Entries from before this field was kept match their request once.
        const bool same = old.value("phase").toString() == phase && old.value("request").toString() == request
            && (!started || !old.contains("started") || old.value("started").toDouble() == *started);
        if (silent || s.muted) {
            // Withdraw the banner but remember the request without a token: if an
            // acknowledgement lapses (an older host, a recovery-only identity) or a
            // schedule ends, the same request must not be announced again.
            current[key] = entry;
            return;
        }
        const auto retry = old.value("retry_at").toInteger();
        if (same && (!retry || retry > now)) {
            auto kept = old;
            if (started) kept["started"] = *started;
            current[key] = kept;
            return;
        }
        QJsonObject target{{"host", host}, {"name", s.name}, {"run", s.runId},
            {"created", s.created}, {"notice", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
        if (reply) { target["reply"] = s.replyId; target["conversation"] = s.conversationId; target["agent"] = s.cmd; }
        const QString token = QString::fromLatin1(QJsonDocument(target).toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        entry["token"] = token;
        current[key] = entry;
        const QString body = concise(s.name, 120) + " / " + concise(host.isEmpty() ? box.host : host, 40)
            + '\n' + reason;
        // Consume foreground edges without changing unread/action state. They
        // must not become delayed banners after focus leaves or the GUI restarts.
        if (!applicationFocused)
            changes.raised.append({token, reply ? QObject::tr("Zerus — New reply") : QObject::tr("Zerus — Needs attention"), body});
    };
    for (const auto &s : box.sessions) {
        const auto baseline = baselines.value(&s);
        // Acknowledged here or on another device: the existing clear path withdraws it.
        if (s.needsAction()) {
            const QString reason = s.phase == "approval" ? QObject::tr("Approval needed")
                : s.phase == "input" ? QObject::tr("Input needed")
                : !s.recovery.value("reason").toString().isEmpty() ? concise(s.recovery.value("reason").toString(), 240)
                : !s.activityDetail.isEmpty() ? concise(s.activityDetail, 240) : QObject::tr("Agent needs attention");
            notice(s, identity(s), s.phase, baselines.contains(&s) ? notificationRequest(s.attentionId, s.attentionStarted) : s.attentionId,
                reason, false, s.attentionAcknowledged || suppressedMain(s, baseline),
                s.attentionId.isEmpty() ? std::optional<double>(s.attentionStarted) : std::nullopt);
        }
        // Read state comes from FleetState, not the raw CLI snapshot. Replies
        // belong to conversations so a rename/resume does not repeat a banner.
        if (s.state != "archived" && s.unreadReply && !s.replyId.isEmpty() && !s.conversationId.isEmpty()) {
            const auto key = "reply:" + s.cmd + ':' + s.conversationId;
            // A main Stop can be a progress reply while background work is
            // still running. Keep it unread, but wait for confirmed idleness
            // before posting a banner. Retain delivered identities below so
            // a working/idle cycle cannot notify again for the same reply.
            const bool idle = s.activity == "idle" && s.phase == "idle" && s.backgroundProcesses <= 0 && s.subagentActiveCount == 0
                && !s.hasCoordinationWait() && !s.needsAction();
            // Hosts do not report routine ticks of a recurring scheduled job as
            // replies; consume any that arrive. The reply after the schedule
            // ends, or once its count is known, notifies as usual.
            if (s.muted || baseline.value("reply_id").toString() == s.replyId || (idle && s.replyScheduled && s.recurringSchedules > 0))
                notice(s, key, "reply", s.replyId, {}, true, true);
            else if (idle && (!s.replyScheduled || s.recurringSchedules == 0))
                notice(s, key, "reply", s.replyId, QObject::tr("Agent has a new reply"), true);
            else if (previous.value(key).toObject().value("request") == s.replyId) current[key] = previous.value(key);
        }
        if (s.state != "running" || s.processState == "exited" || s.activity == "unknown"
            || (s.conversationState == "ended" && s.activity != "idle") || s.subagentSource == "hook_profiles") continue;
        for (auto it = s.subagents.begin(); it != s.subagents.end(); ++it) {
            const auto child = it.value().toObject();
            const auto phase = SessionNotifications::childPhase(child);
            if (phase != "approval" && phase != "input" && phase != "attention") continue;
            const auto request = SessionNotifications::childRequest(child);
            const auto label = child.value("label").toString(child.value("name").toString(it.key()));
            notice(s, "child:" + identity(s) + ':' + it.key(), phase,
                baselines.contains(&s) ? notificationRequest(request, SessionNotifications::childStarted(child)) : request,
                QObject::tr("Subagent %1: %2").arg(concise(label, 100), phase == "approval" ? QObject::tr("Approval needed") : QObject::tr("Input needed")),
                false, s.attentionAcknowledged || suppressedChild(baseline, it.key(), child));
        }
    }
    // Optional questions belong to conversations, like replies. Every listed row
    // keeps its conversation's seen IDs, so a stop and resume stays quiet; only a
    // running one can have pending questions.
    struct Optional { const SessionInfo *session = nullptr; QStringList ids; bool unknown = false; };
    QMap<QString, Optional> optional;
    const auto live = [](const SessionInfo &s) { return s.state == "running" && s.processState != "exited"; };
    for (const auto &s : box.sessions) {
        if (s.conversationId.isEmpty() || !s.optionalQuestionsReported) continue;
        auto &entry = optional["question:" + s.cmd + ':' + s.conversationId];
        if (!entry.session || (live(s) && !live(*entry.session))) entry.session = &s;
        if (live(s) && s.optionalQuestions < 0) entry.unknown = true;
        if (SessionPresentation::optionalQuestionCount(s) > 0)
            for (const auto &id : s.optionalQuestionIds) if (!entry.ids.contains(id)) entry.ids << id;
    }
    QList<AttentionNotice> questions;
    for (auto it = optional.begin(); it != optional.end(); ++it) {
        auto old = previous.value(it.key()).toObject();
        const auto &s = *it->session;
        const auto baseline = baselines.value(&s).value("optional_question_ids").toArray();
        if (!baseline.isEmpty()) {
            const auto posted = target(old.value("token").toString());
            if (posted.value("host").toString() == host && posted.value("agent").toString() == s.cmd
                && posted.value("conversation").toString() == s.conversationId && baseline.contains(posted.value("question"))) {
                old.remove("token"); old.remove("retry_at");
            }
        }
        if (it->unknown) {
            // An unreadable index is not an answer: keep the banner and IDs.
            if (!old.isEmpty()) {
                auto entry = old;
                if (s.muted) { entry.remove("token"); entry.remove("retry_at"); }
                current[it.key()] = entry;
            }
            continue;
        }
        QStringList seen, fresh;
        QSet<QString> known;
        for (const auto &id : old.value("seen").toArray()) {
            known.insert(id.toString());
            if (!it->ids.contains(id.toString())) seen << id.toString();
        }
        // Baseline membership suppresses old questions without evicting a newer
        // observed question from the separately bounded seen-ID history.
        for (const auto &id : baseline) known.insert(id.toString());
        for (const auto &id : it->ids) if (!known.contains(id)) fresh << id;
        seen = seen.mid(qMax<qsizetype>(0, seen.size() + it->ids.size() - SeenQuestions)) + it->ids;
        QJsonObject entry{{"seen", QJsonArray::fromStringList(seen)}};
        // Marking the session read here or on another device acknowledges its
        // questions; turning banners off withdraws them.
        if (!it->ids.isEmpty() && m_optionalQuestions && !s.attentionAcknowledged && !s.muted) {
            const auto token = old.value("token").toString();
            const auto retry = old.value("retry_at").toInteger();
            // A request that needs action announces itself; while Zerus has focus
            // new questions are only recorded, as foreground edges are.
            if ((!fresh.isEmpty() || (!token.isEmpty() && retry && retry <= now)) && !applicationFocused && !s.needsAction()) {
                QJsonObject target{{"host", host}, {"name", s.name}, {"run", s.runId}, {"created", s.created},
                    {"notice", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                    {"question", fresh.isEmpty() ? it->ids.last() : fresh.last()}, {"conversation", s.conversationId}, {"agent", s.cmd}};
                entry["token"] = QString::fromLatin1(QJsonDocument(target).toJson(QJsonDocument::Compact)
                    .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
                const int count = qMax<int>(s.optionalQuestions, it->ids.size());
                questions.append({entry.value("token").toString(), QObject::tr("Zerus — Optional question"),
                    concise(s.name, 120) + " / " + concise(host.isEmpty() ? box.host : host, 40) + '\n'
                    + (count == 1 ? QObject::tr("Optional question waiting") : QObject::tr("%1 optional questions waiting").arg(count))});
            } else if (!token.isEmpty()) {
                entry["token"] = token;
                if (retry) entry["retry_at"] = retry;
            }
        }
        current[it.key()] = entry;
    }
    // Posted first, so on Windows a blocking request from the same poll stays
    // the message whose click is handled.
    changes.raised = questions + changes.raised;
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

bool AttentionTracker::matches(const QJsonObject &target, const SessionInfo &s)
{
    if (target.isEmpty() || s.state == "archived") return false;
    if (!target.value("reply").toString().isEmpty() || !target.value("question").toString().isEmpty())
        return target.value("conversation").toString() == s.conversationId && target.value("agent").toString() == s.cmd;
    const auto run = target.value("run").toString();
    return run.isEmpty() ? target.value("name").toString() == s.name && target.value("created").toInteger() == s.created
                         : run == s.runId;
}
