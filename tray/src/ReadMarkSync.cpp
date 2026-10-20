#include "ReadMarkSync.h"

#include <QDateTime>

#include <algorithm>

#include "HgsClient.h"

ReadMarkSync::ReadMarkSync(HgsClient *client, QObject *parent)
    : QObject(parent), m_client(client), m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
    // Queued: a process that fails to start may report before pump() has
    // recorded which mark the request belongs to.
    connect(m_client, &HgsClient::readFinished, this, &ReadMarkSync::finished, Qt::QueuedConnection);
    m_timer.setInterval(5000);
    connect(&m_timer, &QTimer::timeout, this, &ReadMarkSync::pump);
    m_timer.start();
}

void ReadMarkSync::setPending(const QJsonObject &pending)
{
    // Pending is normally keyed by ReadMark::key() already; this only guards
    // malformed input by merging entries that share a key, never overwriting.
    m_pending = {};
    for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
        const auto mark = ReadMark::fromJson(it.value().toObject());
        const auto key = mark.key();
        const auto existing = m_pending.value(key).toObject();
        m_pending[key] = existing.isEmpty() ? mark.toJson() : ReadMark::merged(ReadMark::fromJson(existing), mark).toJson();
    }
    for (auto it = m_attempts.begin(); it != m_attempts.end();)
        if (!m_pending.contains(it.key()) && !it->inFlight) it = m_attempts.erase(it); else ++it;
    pump();
}

void ReadMarkSync::pump()
{
    const qint64 now = m_clock();
    // A slow or busy host must not hold every slot: each free one goes to the
    // host with the fewest writes in flight, then in key order.
    QHash<QString, int> busy;
    for (const auto &attempt : std::as_const(m_attempts)) if (attempt.inFlight) ++busy[attempt.host];
    while (m_requests.size() < kMaxInFlight) {
        QString chosen; int fewest = kMaxInFlight + 1;
        for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
            const auto entry = it.value().toObject();
            const auto attempt = m_attempts.constFind(it.key());
            if (attempt != m_attempts.constEnd() && (attempt->inFlight || (attempt->sent == entry && now < attempt->dueAt))) continue;
            const auto mark = ReadMark::fromJson(entry);
            if (mark.isEmpty() || mark.name.isEmpty() || mark.runId.isEmpty() || mark.conversationId.isEmpty()) continue;
            if (busy.value(mark.host) < fewest) { fewest = busy.value(mark.host); chosen = it.key(); }
        }
        if (chosen.isEmpty()) return;
        const auto entry = m_pending.value(chosen).toObject();
        const auto mark = ReadMark::fromJson(entry);
        auto &attempt = m_attempts[chosen];
        if (attempt.sent != entry) attempt.failures = attempt.unapplied = 0;
        attempt.sent = entry; attempt.host = mark.host; attempt.inFlight = true; ++busy[mark.host];
        m_requests.insert(m_client->requestRead(mark.host, mark.name, mark.payload()), chosen);
    }
}

void ReadMarkSync::finished(quint64 request, bool delivered, const QJsonObject &result, const QString &error)
{
    const auto key = m_requests.take(request);
    if (key.isEmpty()) return;
    if (!m_pending.contains(key)) {
        m_attempts.remove(key);
        pump();
        return;
    }
    auto &attempt = m_attempts[key];
    attempt.inFlight = false;
    const qint64 now = m_clock();
    if (delivered) {
        // A host that applies only part of a mark (no longer current, or its
        // question index not ready) needs a new snapshot, not the same write.
        const auto sent = ReadMark::fromJson(attempt.sent);
        const auto applied = result.value("applied").toObject();
        const bool all = (sent.replyId.isEmpty() || applied.value("reply_id").toBool())
            && (sent.attentionSignature.isEmpty() || applied.value("attention_signature").toBool());
        attempt.failures = 0;
        attempt.dueAt = now + (all ? kConfirmedResendMs : std::min(kMaxUnappliedResendMs, kConfirmedResendMs << std::min(attempt.unapplied, 3)));
        attempt.unapplied = all ? 0 : attempt.unapplied + 1;
    } else {
        attempt.dueAt = now + std::min(kMaxBackoffMs, kFirstBackoffMs << std::min(attempt.failures, 5));
        ++attempt.failures;
    }
    emit markFinished(key, delivered, result, error);
    pump();
}
