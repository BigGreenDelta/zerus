#pragma once
#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <optional>

// A deliberately closed projection: transport metadata and arbitrary provider
// fields never become display text or diagnostic attachments.
namespace AnalyticsData {
constexpr qsizetype MaximumBytes = 1024 * 1024;
constexpr int MaximumBuckets = 1080;
constexpr int MaximumComputers = 32;
constexpr int PageSize = 50;
QStringList providers();
QStringList counters();
QStringList speechCounters();
bool validRange(QDate from, QDate through);
std::optional<QJsonObject> summary(const QByteArray &bytes, QDate from, QDate through);
std::optional<QJsonObject> speech(const QByteArray &bytes, QDate from, QDate through);

struct Observation {
    QString route;
    QJsonObject agent, voice;
    bool current = false, voiceCurrent = false;
    QString error, voiceError; // Fixed client messages, never remote stderr.
    qint64 voiceReadAt = 0;
};
// Retired source epochs are tombstones for this view's lifetime. A route whose
// stale store generates a newer timestamp cannot revive pre-purge totals.
class EpochGuard {
public:
    bool accept(const QJsonObject &summary);
private:
    struct State { QString epoch; double revision=0, generated=0; QSet<QString> retired; bool blocked=false; };
    QMap<QString,State> m_sources;
};
// Select one whole response per native machine/source, including a newer stale
// epoch rather than reviving pre-purge buckets through another route.
QList<Observation> uniqueComputers(const QList<Observation> &observations);
QJsonObject totals(const QJsonArray &buckets, const QString &provider = {});
// Merge recorded samples, never local means/percentiles. A missing,
// incompatible or overflowing histogram makes that measure unavailable.
QJsonObject histogramTotals(const QJsonArray &buckets, const QString &provider = {});
QJsonObject speechTotals(const QJsonArray &days);
QJsonObject diagnostic(const QList<Observation> &observations, QDate from, QDate through);
}
