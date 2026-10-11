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
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd() && m_requests.size() < kMaxInFlight; ++it) {
        auto &attempt = m_attempts[it.key()];
        const auto entry = it.value().toObject();
        if (attempt.inFlight || (attempt.sent == entry && now < attempt.dueAt)) continue;
        const auto mark = ReadMark::fromJson(entry);
        if (mark.isEmpty() || mark.name.isEmpty() || mark.runId.isEmpty() || mark.conversationId.isEmpty()) continue;
        if (attempt.sent != entry) attempt.failures = 0;
        attempt.sent = entry; attempt.inFlight = true;
        m_requests.insert(m_client->requestRead(mark.host, mark.name, mark.payload()), it.key());
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
        attempt.failures = 0; attempt.dueAt = now + kConfirmedResendMs;
    } else {
        attempt.dueAt = now + std::min(kMaxBackoffMs, kFirstBackoffMs << std::min(attempt.failures, 5));
        ++attempt.failures;
    }
    emit markFinished(key, delivered, result, error);
    pump();
}
