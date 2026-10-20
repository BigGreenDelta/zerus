#include "FleetState.h"
#include "SessionNotifications.h"

#include <QStringList>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <utility>

namespace {

int visibleSessionCount(const BoxState &box)
{
    return int(std::count_if(box.sessions.cbegin(), box.sessions.cend(),
                            [](const SessionInfo &session) { return session.state != QLatin1String("archived"); }));
}

// "just now" / "5m ago" — готовая фраза, без разбивки на число и единицу, потому что
// "just now" не про число.
//
// Минуты, а не секунды, и "just now" на полторы минуты. Возраст отвечает на вопрос "ноут
// спит или потеряли контакт" (см. setPeer), секунды ему не нужны -- а каждая смена текста
// это новый тултип у иконки, на маке -- обращение к NSStatusItem, которых должно быть как
// можно меньше (см. QuietTrayIcon.h). С секундами текст менялся почти на каждом
// 2-секундном тике: пир опрашивается раз в минуту, возраст всегда моложе минуты.
// Полторы минуты покрывают штатный минутный опрос вместе с задержкой ssh, так что
// здоровый пир не мигает "1m ago" перед каждым ответом; пропавший честно считает минуты.
QString formatAge(qint64 deltaMs)
{
    if (deltaMs < 0)
        deltaMs = 0;
    const qint64 sec = deltaMs / 1000;
    if (sec < 90)
        return QStringLiteral("just now");
    return QStringLiteral("%1m ago").arg(sec / 60);
}

} // namespace

namespace {
QString replyKey(const QString &host, const SessionInfo &s) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{host, s.cmd, s.conversationId}).toJson(QJsonDocument::Compact));
}
QString attentionFallbackKey(const QString &host, const SessionInfo &s) {
    return QString::fromUtf8(QJsonDocument(QJsonArray{host, s.cmd, s.name, s.created}).toJson(QJsonDocument::Compact));
}
QString attentionKey(const QString &host, const SessionInfo &s) {
    return s.conversationId.isEmpty() ? attentionFallbackKey(host, s) : replyKey(host, s);
}
QString attentionFingerprint(const SessionInfo &s) {
    QJsonObject children;
    for (auto it = s.subagents.begin(); s.subagentSource != "hook_profiles" && it != s.subagents.end(); ++it) {
        const auto child = it.value().toObject();
        const auto phase = child.value("display_state").toString(child.value("state").toString());
        if (phase != "approval" && phase != "input" && phase != "attention") continue;
        const auto request = child.value("attention_id").toString(child.value("question_id").toString());
        children[it.key()] = QJsonArray{phase, request,
            request.isEmpty() ? child.value("last_event_at") : QJsonValue()};
    }
    // A native request stays the same across transcript/status updates. Keep a
    // timestamp fallback only for providers without a concrete request identity.
    return QString::fromUtf8(QJsonDocument(QJsonArray{s.runId,
        s.needsAction() ? s.phase : QString(), s.needsAction() ? s.attentionId : QString(),
        s.needsAction() && s.attentionId.isEmpty() ? QJsonValue(s.lastEventAt) : QJsonValue(), children})
        .toJson(QJsonDocument::Compact));
}

// A successful snapshot reports null optional questions while the host's
// question index is incomplete or unreadable. Keep the last value listed for the
// same conversation, whatever the row state, so the chip does not flicker;
// another conversation starts unknown. Question IDs stay stable across runs.
void keepOptionalQuestions(QList<SessionInfo> &sessions, const QList<SessionInfo> &before)
{
    for (auto &s : sessions) {
        if (!s.optionalQuestionsReported || s.optionalQuestions >= 0) continue;
        for (const auto &old : before) {
            if (old.name != s.name || old.archiveId != s.archiveId
                || old.conversationId != s.conversationId || old.optionalQuestions < 0) continue;
            s.optionalQuestions = old.optionalQuestions; s.optionalQuestionIds = old.optionalQuestionIds;
            s.optionalQuestionsLatestAt = old.optionalQuestionsLatestAt;
            break;
        }
    }
}

// A poll started before a confirmed write must not roll its host revision back.
// No local preference is created: only values already confirmed by that host
// can bridge an older, otherwise supported snapshot of the same conversation.
void keepNotificationPreferences(QList<SessionInfo> &sessions, const QList<SessionInfo> &before)
{
    QHash<QString, const SessionInfo *> known;
    for (const auto &old : before) if (SessionNotifications::valid(old.notificationPreferences, old.conversationId)) {
        const auto key = QString::fromUtf8(QJsonDocument(QJsonArray{old.cmd, old.conversationId}).toJson(QJsonDocument::Compact)); // Host-scoped by the caller.
        const auto found = known.value(key);
        if (!found || SessionNotifications::revision(found->notificationPreferences.value("revision"))
                < SessionNotifications::revision(old.notificationPreferences.value("revision"))) known[key] = &old;
    }
    for (auto &next : sessions) {
        const auto key = QString::fromUtf8(QJsonDocument(QJsonArray{next.cmd, next.conversationId}).toJson(QJsonDocument::Compact));
        const auto old = known.value(key); if (!old) continue;
        if (!next.sharedNotifications) {
            // Keep the last host-confirmed display through downgrade/unknown
            // snapshots, while leaving command authority explicitly disabled.
            next.notificationPreferences = old->notificationPreferences; next.muted = old->muted;
            continue;
        }
        const auto previous = SessionNotifications::revision(old->notificationPreferences.value("revision"));
        const auto incoming = SessionNotifications::revision(next.notificationPreferences.value("revision"));
        if (incoming > previous || (incoming == previous && next.notificationPreferences == old->notificationPreferences)) continue;
        const bool conflict = incoming == previous;
        next.notificationPreferences = old->notificationPreferences; next.muted = old->muted;
        if (conflict) next.notificationPreferencesError = QStringLiteral("Conflicting shared notification preferences. Refresh the session.");
    }
}

// A queued mark is finished once the host shows it, or once none of its values
// is current any more: the host would report every field stale.
bool settled(const ReadMark &mark, const SessionInfo &s) {
    if (mark.runId != s.runId || mark.conversationId != s.conversationId) return true;
    const bool reply = mark.replyId.isEmpty() || mark.replyId != s.replyId || s.readReplyId == mark.replyId;
    const bool attention = mark.attentionSignature.isEmpty() || mark.attentionSignature != s.attentionSignature
        || s.readAttentionSignature == mark.attentionSignature;
    return reply && attention;
}
}
void FleetState::updateUnread(const QString &host, BoxState &box)
{
    QSet<QString> listed;
    for (auto &s : box.sessions) {
        const auto key = attentionKey(host, s), fallback = attentionFallbackKey(host, s);
        if (key != fallback && m_attentionMarks.contains(fallback)) {
            if (!m_attentionMarks.contains(key)) m_attentionMarks[key] = m_attentionMarks.value(fallback);
            m_attentionMarks.remove(fallback);
        }
        const auto mark = m_attentionMarks.value(key).toObject();
        s.reviewLater = s.state != "archived" && mark.value("review").toBool();
        const bool localAcknowledged = mark.value("acknowledged").toString() == attentionFingerprint(s);
        if (!s.sharedRead) {
            // Hosts with an older hgs keep this device's own read state.
            s.unreadReply = s.state != "archived" && !s.conversationId.isEmpty() && !s.replyId.isEmpty()
                && m_readReplies.value(replyKey(host, s)).toString() != s.replyId;
            s.attentionAcknowledged = localAcknowledged;
            continue;
        }
        const auto readKey = ReadMark{host, s.name, {}, s.conversationId, {}, {}}.key();
        listed.insert(readKey);
        auto queued = ReadMark::fromJson(m_pendingReads.value(readKey).toObject());
        if (m_pendingReads.contains(readKey) && settled(queued, s)) {
            m_settledReads << queued; m_pendingReads.remove(readKey); queued = {};
        }
        const bool sameRun = !queued.runId.isEmpty() && queued.runId == s.runId && queued.conversationId == s.conversationId;
        const bool replyRead = s.readReplyId == s.replyId || (sameRun && queued.replyId == s.replyId);
        s.unreadReply = s.state != "archived" && !s.conversationId.isEmpty() && !s.replyId.isEmpty() && !replyRead;
        // Recovery-only attention has no host signature and stays a local choice.
        s.attentionAcknowledged = s.attentionSignature.isEmpty() ? localAcknowledged
            : s.attentionSignature == s.readAttentionSignature || (sameRun && queued.attentionSignature == s.attentionSignature);
    }
    if (!box.ok) return;
    // Marks for sessions this host no longer lists can never apply.
    for (auto it = m_pendingReads.begin(); it != m_pendingReads.end();) {
        const auto queued = ReadMark::fromJson(it.value().toObject());
        if (queued.host == host && !listed.contains(it.key())) { m_settledReads << queued; it = m_pendingReads.erase(it); }
        else ++it;
    }
}
void FleetState::setAttentionMarks(const QJsonObject &marks)
{
    m_attentionMarks = marks; updateUnread({}, m_local);
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) updateUnread(it.key(), it->box);
}
SessionInfo *FleetState::matchingSession(const QString &host, const SessionInfo &expected)
{
    BoxState *box = host.isEmpty() ? &m_local : m_peers.contains(host) ? &m_peers[host].box : nullptr;
    if (!box) return nullptr;
    for (auto &s : box->sessions)
        if (s.name == expected.name && s.archiveId == expected.archiveId && s.conversationId == expected.conversationId
            && s.created == expected.created && s.state != "archived") return &s;
    return nullptr;
}
bool FleetState::setReviewLater(const QString &host, const SessionInfo &expected, bool marked)
{
    auto *s = matchingSession(host, expected); if (!s) return false;
    const auto key = attentionKey(host, *s);
    auto mark = m_attentionMarks.value(key).toObject(); mark["review"] = marked;
    m_attentionMarks[key] = mark; s->reviewLater = marked; return true;
}
bool FleetState::applyNotificationPreferences(const QString &host, const SessionInfo &expected, const QJsonObject &preferences)
{
    auto *s = matchingSession(host, expected);
    if (!s || !s->sharedNotifications || s->runId.isEmpty() || s->runId != expected.runId
        || !SessionNotifications::valid(preferences, s->conversationId)) return false;
    const auto current = SessionNotifications::revision(s->notificationPreferences.value("revision"));
    const auto incoming = SessionNotifications::revision(preferences.value("revision"));
    if (incoming < current || (incoming == current && preferences != s->notificationPreferences)) return false;
    s->notificationPreferences = preferences; s->muted = preferences.value("muted").toBool();
    s->notificationPreferencesError.clear(); return true;
}
bool FleetState::markSessionRead(const QString &host, const SessionInfo &expected)
{
    auto *s = matchingSession(host, expected);
    if (!s || s->replyId != expected.replyId || attentionFingerprint(*s) != attentionFingerprint(expected)
        || s->attentionSignature != expected.attentionSignature) return false;
    queueRead(host, *s, true, true);
    markReplyRead(host, s->name, s->conversationId, s->replyId);
    const auto key = attentionKey(host, *s);
    auto mark = m_attentionMarks.value(key).toObject();
    mark["review"] = false; mark["acknowledged"] = attentionFingerprint(*s);
    m_attentionMarks[key] = mark; s->reviewLater = false; s->attentionAcknowledged = true; return true;
}
void FleetState::setReadReplies(const QJsonObject &read)
{
    m_readReplies = read; updateUnread({}, m_local);
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) updateUnread(it.key(), it->box);
}
QJsonObject FleetState::unreadReplies() const
{
    QJsonObject replies;
    const auto collect = [&](const QString &host, const BoxState &box) {
        for (const auto &s : box.sessions)
            if (s.unreadReply && !s.reviewLater) replies[replyKey(host, s)] = s.replyId;
    };
    collect({}, m_local);
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) collect(it.key(), it->box);
    return replies;
}
int FleetState::markRepliesRead(const QJsonObject &replies)
{
    int marked = 0;
    const auto mark = [&](const QString &host, BoxState &box) {
        for (const auto &s : box.sessions) {
            // A session marked as needing attention keeps its reply unread, even when
            // the mark was added after the UI took its snapshot.
            if (!s.unreadReply || s.reviewLater) continue;
            const auto key = replyKey(host, s);
            // A newer poll may have arrived since the UI took its snapshot.
            // Acknowledging old replies must never acknowledge the next answer.
            if (replies.value(key).toString() != s.replyId) continue;
            if (!s.sharedRead && m_readReplies.value(key).toString() == s.replyId) continue;
            m_readReplies[key] = s.replyId; queueRead(host, s, true, false); ++marked;
        }
        updateUnread(host, box);
    };
    mark({}, m_local);
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) mark(it.key(), it->box);
    return marked;
}
bool FleetState::markReplyRead(const QString &host, const QString &name, const QString &conversation, const QString &reply)
{
    if (reply.isEmpty() || conversation.isEmpty()) return false;
    BoxState *box = host.isEmpty() ? &m_local : m_peers.contains(host) ? &m_peers[host].box : nullptr;
    if (!box) return false;
    for (auto &s : box->sessions) {
        if (s.state == "archived" || s.name != name || s.conversationId != conversation || s.replyId != reply || !s.unreadReply) continue;
        m_readReplies[replyKey(host, s)] = reply; queueRead(host, s, true, false);
        updateUnread(host, *box); return true;
    }
    return false;
}

void FleetState::setLocal(const BoxState &box, qint64 /*nowMs*/)
{
    // Свой бокс не имеет понятия "опрошен давно": мы и есть этот бокс, вызов hgs
    // локальный и синхронный по сути (миллисекунды). localIsAlwaysFresh() полагается
    // на то, что isStale() вообще не смотрит на m_local.
    BoxState next = box; keepOptionalQuestions(next.sessions, m_local.sessions); keepNotificationPreferences(next.sessions, m_local.sessions);
    m_local = std::move(next); updateUnread({}, m_local);
    m_haveLocal = true;
    if (box.peersKnown) {
        for (auto it = m_peers.begin(); it != m_peers.end(); ) {
            if (!box.peers.contains(it.key())) it = m_peers.erase(it); else ++it;
        }
        for (const auto &peer : box.peers) registerPeer(peer);
    }
}

void FleetState::registerPeer(const QString &alias)
{
    // operator[] на QMap создаёт запись со значениями по умолчанию, если её ещё нет;
    // если пир уже был опрошен раньше — не затираем его состояние повторной регистрацией.
    m_peers[alias];
}

void FleetState::setPeer(const BoxState &box, qint64 nowMs)
{
    // A late reply cannot resurrect a profile removed/disabled while polling.
    if (m_local.peersKnown && !m_local.peers.contains(box.host)) return;
    PeerEntry &entry = m_peers[box.host];

    if (box.ok) {
        const auto before = std::exchange(entry.box.sessions, {});
        entry.box = box; keepOptionalQuestions(entry.box.sessions, before); keepNotificationPreferences(entry.box.sessions, before); updateUnread(box.host, entry.box);
    } else {
        // Пир ответил "я офлайн/сплю" — это не то же самое, что "мы не знаем, что там".
        // hgs в таком случае не может опросить tmux и присылает sessions:[], но реальные
        // сессии на ноуте никуда не делись. Берём из нового ответа только то, что он
        // ДЕЙСТВИТЕЛЬНО знает (host/ok/error), и оставляем sessions/projects от последнего
        // успешного опроса — иначе totalSessions() соврёт вниз ровно в момент засыпания.
        entry.box.host = box.host;
        entry.box.ok = false;
        entry.box.error = box.error;
    }

    // polledAt обновляется всегда, даже на ok:false: мы СПРОСИЛИ и получили осмысленный
    // ответ (пир жив и сказал "не сейчас"), это не то же самое, что молчание/таймаут
    // самого hgs (тот идёт через HgsClient::failed и вообще не долетает до setPeer).
    // Только так тултип может отличить "спросили только что, недоступен" от "не спрашивали
    // час" — см. tooltip().
    entry.polledAt = nowMs;
    entry.everPolled = true;
}

int FleetState::totalSessions() const
{
    int total = m_haveLocal ? visibleSessionCount(m_local) : 0;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        // Не фильтруем по box.ok: последний известный список сессий переживает переход
        // в ok:false (см. setPeer) именно затем, чтобы здесь его не пришлось откидывать.
        // Фильтр только по everPolled — пока пира не опросили ни разу, его сессии
        // неизвестны, а не нулевые, и складывать в сумму нечего.
        if (it->everPolled)
            total += visibleSessionCount(it->box);
    }
    return total;
}

bool FleetState::isStale(qint64 nowMs) const
{
    // Свой бокс никогда не протухает: опрос локальный и всегда актуален на момент вызова.
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        const PeerEntry &e = it.value();
        if (!e.everPolled)
            return true;
        if (nowMs - e.polledAt > kPeerStaleMs)
            return true;
        if (!e.box.ok)
            return true;
    }
    return false;
}

int FleetState::attentionSessions(qint64 nowMs) const
{
    auto count = [](const BoxState &box) {
        return int(std::count_if(box.sessions.cbegin(), box.sessions.cend(),
            [&box](const SessionInfo &s) { return !s.muted && (s.reviewLater || (box.ok && s.needsAttention())); }));
    };
    int total = m_haveLocal ? count(m_local) : 0;
    for (const auto &entry : m_peers)
        if (entry.everPolled) {
            if (nowMs - entry.polledAt < kPeerStaleMs) total += count(entry.box);
            else total += int(std::count_if(entry.box.sessions.cbegin(), entry.box.sessions.cend(),
                [](const SessionInfo &s) { return !s.muted && s.reviewLater; }));
        }
    return total;
}

QString FleetState::tooltip(qint64 nowMs) const
{
    QStringList lines;
    const int attention = attentionSessions(nowMs), total = totalSessions();
    lines << (attention == 1 ? QStringLiteral("1 needs attention") : QStringLiteral("%1 need attention").arg(attention))
                 + (total == 1 ? QStringLiteral(", 1 session total") : QStringLiteral(", %1 sessions total").arg(total));
    // A control-only computer has no sessions of its own to count.
    if (m_local.localSessions)
        lines << QStringLiteral("%1 here").arg(m_haveLocal ? visibleSessionCount(m_local) : 0);

    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        const QString &alias = it.key();
        const PeerEntry &e = it.value();

        if (!e.everPolled) {
            lines << QStringLiteral("%1: not polled yet").arg(alias);
            continue;
        }

        const QString age = formatAge(nowMs - e.polledAt);
        if (!e.box.ok) {
            // "unreachable" само по себе не отвечает на вопрос "а мы вообще спрашивали
            // недавно?" — а это ровно то, чем спящий ноут (свежий отказ) отличается от
            // потерянного контакта (старый отказ). Возраст последнего опроса — тот
            // самый ответ.
            lines << QStringLiteral("%1: unreachable (checked %2)").arg(alias, age);
            continue;
        }

        lines << QStringLiteral("%1 on %2 (%3)").arg(visibleSessionCount(e.box)).arg(alias, age);
    }

    return lines.join(QLatin1Char('\n'));
}

QList<QString> FleetState::peerNames() const
{
    return m_peers.keys();
}

const BoxState *FleetState::peer(const QString &alias) const
{
    const auto it = m_peers.constFind(alias);
    if (it == m_peers.constEnd())
        return nullptr;
    return &it->box;
}

qint64 FleetState::peerPolledAt(const QString &alias) const
{
    const auto it = m_peers.constFind(alias);
    if (it == m_peers.constEnd())
        return 0;
    return it->polledAt;
}

void FleetState::queueRead(const QString &host, const SessionInfo &s, bool reply, bool attention)
{
    if (!s.sharedRead || s.state == "archived" || s.runId.isEmpty() || s.conversationId.isEmpty()) return;
    ReadMark mark{host, s.name, s.runId, s.conversationId,
        reply && s.readReplyId != s.replyId ? s.replyId : QString(),
        attention && s.readAttentionSignature != s.attentionSignature ? s.attentionSignature : QString()};
    if (mark.isEmpty()) return;
    mark = ReadMark::merged(ReadMark::fromJson(m_pendingReads.value(mark.key()).toObject()), mark);
    m_pendingReads[mark.key()] = mark.toJson(); m_newReads << mark;
}
void FleetState::setPendingReads(const QJsonObject &pending)
{
    m_pendingReads = pending; updateUnread({}, m_local);
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) updateUnread(it.key(), it->box);
}
QList<ReadMark> FleetState::takeNewReadMarks() { return std::exchange(m_newReads, {}); }
QList<ReadMark> FleetState::takeSettledReadMarks() { return std::exchange(m_settledReads, {}); }
bool FleetState::supportsSharedRead(const QString &host) const
{
    const BoxState *box = host.isEmpty() ? &m_local : peer(host);
    return box && box->ok && std::any_of(box->sessions.cbegin(), box->sessions.cend(),
        [](const SessionInfo &s) { return s.sharedRead; });
}
QList<ReadMark> FleetState::legacyReadMarks(const QString &host) const
{
    QList<ReadMark> marks;
    const BoxState *box = host.isEmpty() ? &m_local : peer(host);
    if (!box || !box->ok) return marks;
    for (const auto &s : box->sessions) {
        if (!s.sharedRead || s.state == "archived" || s.runId.isEmpty() || s.conversationId.isEmpty()) continue;
        // Only marks for the current values can still apply on the host.
        ReadMark mark{host, s.name, s.runId, s.conversationId, {}, {}};
        if (!s.replyId.isEmpty() && s.readReplyId != s.replyId && m_readReplies.value(replyKey(host, s)).toString() == s.replyId)
            mark.replyId = s.replyId;
        if (!s.attentionSignature.isEmpty() && s.readAttentionSignature != s.attentionSignature
            && m_attentionMarks.value(attentionKey(host, s)).toObject().value("acknowledged").toString() == attentionFingerprint(s))
            mark.attentionSignature = s.attentionSignature;
        if (!mark.isEmpty()) marks << mark;
    }
    return marks;
}
