#include "AnalyticsData.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace AnalyticsData {
namespace {
constexpr qint64 MaximumInteger = 9007199254740991LL;
constexpr double MaximumNumber = double(MaximumInteger); // exact JSON integers
bool number(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0
        && value.toDouble() <= MaximumNumber && std::floor(value.toDouble()) == value.toDouble();
}
bool timestamp(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() < 253402300800.;
}
bool uuid(const QJsonValue &value, bool allowNil = false) {
    const auto text = value.toString(); const QUuid id(text);
    return (!id.isNull() || allowNil) && id.toString(QUuid::WithoutBraces) == text;
}
std::optional<QJsonObject> object(const QByteArray &bytes) {
    if (bytes.size() > MaximumBytes) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().value("schema") != 1) return {};
    return document.object();
}
std::optional<QJsonArray> codes(const QJsonValue &value, const QStringList &allowed) {
    if (!value.isArray() || value.toArray().size() > 32) return {};
    QJsonArray result;
    for (const auto &code : value.toArray()) {
        if (!code.isString()) return {};
        const auto safe = allowed.contains(code.toString()) ? code.toString() : QStringLiteral("unknown");
        if (!result.contains(safe)) result.append(safe);
    }
    return result;
}
bool add(QJsonObject &into, const QString &key, const QJsonValue &value, bool duration = false) {
    if (duration ? (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0 || value.toDouble() > MaximumNumber) : !number(value)) return false;
    const auto sum = into.value(key).toDouble() + value.toDouble();
    if (sum > MaximumNumber) return false;
    into.insert(key, sum); return true;
}
QStringList histogramNames() { return {"work_ms", "approval_wait_ms", "input_wait_ms"}; }
std::optional<qint64> added(qint64 a, qint64 b) {
    if (b > MaximumInteger - a) return {};
    return a + b;
}
std::optional<QJsonObject> histogram(const QJsonValue &value) {
    if (!value.isObject()) return {};
    const auto raw = value.toObject();
    const QJsonArray bounds{100, 1000, 10000, 60000, 600000};
    const auto counts = raw.value("counts").toArray();
    if (raw.value("version") != 1 || raw.value("bounds") != bounds || counts.size() != 6
        || !number(raw.value("count")) || !number(raw.value("sum"))) return {};
    qint64 count = 0;
    for (const auto &n : counts) {
        if (!number(n)) return {};
        const auto total = added(count, n.toInteger());
        if (!total) return {};
        count = *total;
    }
    if (count != raw.value("count").toInteger() || (count == 0 && raw.value("sum").toInteger() != 0)) return {};
    return QJsonObject{{"version",1},{"bounds",bounds},{"counts",counts},{"count",count},{"sum",raw.value("sum")}};
}
// Inputs have passed histogram(), so every component is an exact nonnegative
// integer. Keep all bins, count and sum from the same successful merge.
std::optional<QJsonObject> mergedHistogram(const QJsonObject &a, const QJsonObject &b) {
    const auto count = added(a.value("count").toInteger(), b.value("count").toInteger());
    const auto sum = added(a.value("sum").toInteger(), b.value("sum").toInteger());
    if (!count || !sum) return {};
    const auto left = a.value("counts").toArray(), right = b.value("counts").toArray();
    QJsonArray counts;
    for (int i = 0; i < left.size(); ++i) {
        const auto total = added(left[i].toInteger(), right[i].toInteger());
        if (!total) return {};
        counts.append(*total);
    }
    return QJsonObject{{"version",1},{"bounds",a.value("bounds")},{"counts",counts},{"count",*count},{"sum",*sum}};
}
QJsonObject publicAgent(QJsonObject agent) {
    QJsonObject safe;
    for (const auto &key : {"schema", "source", "revision", "generated_at", "range", "collection", "coverage", "unresolved_waits"}) if (agent.contains(key)) safe.insert(key,agent.value(key));
    safe.insert("reported_totals",totals(agent.value("buckets").toArray()));
    return safe;
}
}

QStringList providers() { return {"codex", "claude", "kimi", "dsh", "opencode", "other"}; }
QStringList counters() {
    return {"turn_started", "turn_completed", "turn_interrupted", "work_ms", "approval_wait_ms", "input_wait_ms", "approval_requested", "approval_resolved", "input_requested", "input_resolved",
            "delivery_submitted", "delivery_failed", "delivery_uncertain", "recovery_attempt", "recovery_succeeded",
            "recovery_exhausted", "failure_authentication", "failure_rate_limit", "failure_quota", "failure_policy",
            "failure_network", "failure_service", "failure_unknown", "input_tokens", "output_tokens", "cache_tokens", "reasoning_tokens"};
}
QStringList speechCounters() { return {"submitted", "completed", "failed", "cancelled", "uncertain", "audio_ms", "queue_ms", "processing_ms"}; }
bool validRange(QDate from, QDate through) { return from.isValid() && through.isValid() && from <= through && from.daysTo(through) < 90; }

std::optional<QJsonObject> summary(const QByteArray &bytes, QDate from, QDate through) {
    if (!validRange(from, through)) return {};
    const auto parsed = object(bytes); if (!parsed) return {};
    const auto raw = *parsed, range = raw.value("range").toObject(), collection = raw.value("collection").toObject(), coverage = raw.value("coverage").toObject();
    if (!uuid(raw.value("machine_id")) || !uuid(raw.value("source_epoch"),true) || raw.value("source") != "agent"
        || !number(raw.value("revision")) || !timestamp(raw.value("generated_at"))
        || range.value("from") != from.toString(Qt::ISODate) || range.value("through") != through.toString(Qt::ISODate)
        || !collection.value("enabled").isBool() || collection.value("event_retention_days") != 14
        || !number(collection.value("retention_days")) || collection.value("retention_days").toInt() < 1 || collection.value("retention_days").toInt() > 90
        || !coverage.value("partial").isBool() || !raw.value("buckets").isArray() || raw.value("buckets").toArray().size() > MaximumBuckets) return {};
    for (const auto &key : {"start", "end"})
        if (!coverage.contains(key) || (!coverage.value(key).isNull() && !timestamp(coverage.value(key)))) return {};
    if (coverage.value("start").isDouble() && coverage.value("end").isDouble() && coverage.value("start").toDouble() > coverage.value("end").toDouble()) return {};
    const auto gaps = codes(coverage.value("gaps"), {"collection_disabled", "history_purged", "retention_boundary", "quota_exhausted", "observation_gap", "clock_discontinuity", "missing_identity", "unsupported_provider"});
    const QStringList measures{"turns", "work_ms", "approval_wait_ms", "input_wait_ms", "delivery", "recovery", "failures", "tokens", "cost", "background_jobs"};
    const auto unsupported = codes(coverage.value("unsupported"), measures);
    if (!gaps || !unsupported) return {};
    QJsonObject providerCoverage;
    for (const auto &provider : providers()) {
        const auto reported = coverage.value("providers").toObject().value(provider).toObject();
        const auto supported = codes(reported.value("supported"),measures), missing = codes(reported.value("unsupported"),measures);
        if (!supported || !missing || !reported.value("partial").isBool()) continue;
        providerCoverage.insert(provider,QJsonObject{{"supported",*supported},{"unsupported",*missing},{"partial",reported.value("partial")}});
    }
    QJsonArray buckets; QSet<QString> keys;
    for (const auto &value : raw.value("buckets").toArray()) {
        const auto row = value.toObject();
        const auto date = QDate::fromString(row.value("date").toString(), Qt::ISODate);
        const auto provider = row.value("provider").toString(), producer = row.value("producer").toString();
        const auto key = date.toString(Qt::ISODate) + '/' + provider + '/' + producer;
        if (!date.isValid() || row.value("date") != date.toString(Qt::ISODate) || date < from || date > through
            || !providers().contains(provider) || (producer != "main" && producer != "child") || keys.contains(key)
            || !row.value("counters").isObject()) return {};
        keys.insert(key); QJsonObject kept;
        for (const auto &name : counters()) if (row.value("counters").toObject().contains(name)) {
            const auto count = row.value("counters").toObject().value(name);
            if (!number(count)) return {}; kept.insert(name, count);
        }
        QJsonObject histograms;
        const auto reportedHistograms = row.value("histograms").toObject();
        for (const auto &name : histogramNames()) {
            if (!reportedHistograms.contains(name)) continue;
            const auto keptHistogram = histogram(reportedHistograms.value(name));
            if (!keptHistogram) return {};
            histograms.insert(name, *keptHistogram);
        }
        buckets.append(QJsonObject{{"date",date.toString(Qt::ISODate)},{"provider",provider},{"producer",producer},{"counters",kept},{"histograms",histograms}});
    }
    QJsonObject result{{"schema",1},{"machine_id",raw.value("machine_id")},{"source","agent"},{"source_epoch",raw.value("source_epoch")},
        {"revision",raw.value("revision")},{"generated_at",raw.value("generated_at")},{"range",QJsonObject{{"from",from.toString(Qt::ISODate)},{"through",through.toString(Qt::ISODate)}}},
        {"collection",QJsonObject{{"enabled",collection.value("enabled")},{"event_retention_days",14},{"retention_days",collection.value("retention_days")}}},
        {"coverage",QJsonObject{{"start",coverage.value("start")},{"end",coverage.value("end")},{"gaps",*gaps},{"unsupported",*unsupported},{"partial",coverage.value("partial")},{"providers",providerCoverage}}},{"buckets",buckets}};
    const auto waits=raw.value("unresolved_waits").toObject();
    if(number(waits.value("observed"))&&waits.value("complete").isBool())result.insert("unresolved_waits",QJsonObject{{"observed",waits.value("observed")},{"complete",waits.value("complete")}});
    return result;
}

std::optional<QJsonObject> speech(const QByteArray &bytes, QDate from, QDate through) {
    const auto parsed = object(bytes); if (!validRange(from, through) || !parsed) return {};
    const auto raw = *parsed;
    if (!raw.value("enabled").isBool() || raw.value("retention_days") != 90 || !raw.value("days").isArray()
        || raw.value("days").toArray().size() > 2880) return {};
    QMap<QString, QJsonObject> dates;
    // Profile IDs are read only to reject duplicate source buckets and are never
    // retained in the projection. They are not telemetry dimensions.
    QSet<QString> keys;
    for (const auto &value : raw.value("days").toArray()) {
        const auto row = value.toObject(); const auto date = QDate::fromString(row.value("day").toString(), Qt::ISODate);
        const auto profile = row.value("profile_id").toString();
        if (!date.isValid() || profile.isEmpty() || profile.size() > 256) return {};
        const auto key = date.toString(Qt::ISODate) + '/' + profile;
        if (keys.contains(key)) return {}; keys.insert(key);
        if (date < from || date > through) continue;
        auto counters = dates.value(date.toString(Qt::ISODate));
        for (const auto &name : speechCounters()) if (!add(counters,name,row.value(name),name.endsWith("_ms"))) return {};
        // Existing service error codes are provider-defined; only these reviewed
        // fixed categories leave the read. Everything else becomes unknown.
        const QStringList allowed{"http_authentication", "http_rate_limited", "http_status", "http_response_invalid", "http_address_blocked", "http_timeout", "http_connection", "inference_failed", "service_restarted"};
        auto errors = counters.value("error_codes").toObject(); const auto rawErrors = row.value("error_codes").toObject();
        if (!row.value("error_codes").isObject() || rawErrors.size() > 64) return {};
        for (auto i = rawErrors.begin(); i != rawErrors.end(); ++i) if (!add(errors, allowed.contains(i.key()) ? i.key() : QStringLiteral("unknown"), i.value())) return {};
        counters.insert("error_codes", errors); dates.insert(date.toString(Qt::ISODate), counters);
    }
    QJsonArray days;
    for (auto i = dates.begin(); i != dates.end(); ++i) { auto row = i.value(); row.insert("date",i.key()); days.append(row); }
    return QJsonObject{{"schema",1},{"enabled",raw.value("enabled")},{"retention_days",90},{"days",days}};
}

bool EpochGuard::accept(const QJsonObject &summary) {
    const auto machine = summary.value("machine_id").toString(), epoch = summary.value("source_epoch").toString();
    const auto revision = summary.value("revision").toDouble(), generated = summary.value("generated_at").toDouble();
    if (!m_sources.contains(machine)) {
        if (m_sources.size() >= MaximumComputers) return false;
        State state;state.epoch=epoch;state.revision=revision;state.generated=generated;m_sources.insert(machine,state);return true;
    }
    auto &state=m_sources[machine];
    if (state.blocked || state.retired.contains(epoch)) return false;
    if (state.epoch==epoch) {
        if (revision<state.revision) return false;
        state.revision=revision;state.generated=qMax(state.generated,generated);return true;
    }
    if (generated<=state.generated) return false;
    if (state.retired.size()>=64) { state.blocked=true;return false; }
    state.retired.insert(state.epoch);state.epoch=epoch;state.revision=revision;state.generated=generated;return true;
}

QList<Observation> uniqueComputers(const QList<Observation> &observations) {
    QMap<QString, Observation> selected;
    for (const auto &observation : observations.mid(0, MaximumComputers)) {
        if (observation.agent.isEmpty()) continue;
        const auto key = observation.agent.value("machine_id").toString() + '/' + observation.agent.value("source").toString();
        const auto old = selected.value(key);
        const auto &next = observation.agent;
        const bool sameEpoch = old.agent.value("source_epoch") == next.value("source_epoch");
        const bool newer = sameEpoch ? next.value("revision").toDouble() > old.agent.value("revision").toDouble()
                                    : next.value("generated_at").toDouble() > old.agent.value("generated_at").toDouble();
        const bool equal = sameEpoch && next.value("revision") == old.agent.value("revision");
        if (old.agent.isEmpty() || newer || (equal && (observation.current > old.current
            || (observation.current == old.current && next.value("generated_at").toDouble() > old.agent.value("generated_at").toDouble())))) selected.insert(key,observation);
    }
    return selected.values();
}

QJsonObject totals(const QJsonArray &buckets, const QString &provider) {
    QJsonObject result; QSet<QString> unavailable;
    for (const auto &value : buckets) {
        const auto row = value.toObject(); if (!provider.isEmpty() && row.value("provider") != provider) continue;
        const auto counts = row.value("counters").toObject();
        for (const auto &name : counters()) {
            if (!counts.contains(name) || !add(result,name,counts.value(name))) unavailable.insert(name);
        }
    }
    for (const auto &name : unavailable) result.remove(name);
    return result;
}

QJsonObject histogramTotals(const QJsonArray &buckets, const QString &provider) {
    QJsonObject result; QSet<QString> unavailable;
    for (const auto &value : buckets) {
        const auto row = value.toObject();
        if (!provider.isEmpty() && row.value("provider") != provider) continue;
        const auto reported = row.value("histograms").toObject();
        for (const auto &name : histogramNames()) {
            if (unavailable.contains(name)) continue;
            auto next = histogram(reported.value(name));
            if (next && result.contains(name)) next = mergedHistogram(result.value(name).toObject(), *next);
            if (next) result.insert(name, *next);
            else { unavailable.insert(name); result.remove(name); }
        }
    }
    return result;
}

QJsonObject speechTotals(const QJsonArray &days) {
    QJsonObject result,errors;QSet<QString> unavailable,unavailableErrors;
    for (const auto &value:days) {
        const auto day=value.toObject();
        for (const auto &name:speechCounters()) if(!add(result,name,day.value(name),name.endsWith("_ms")))unavailable.insert(name);
        const auto typed=day.value("error_codes").toObject();
        for(auto i=typed.begin();i!=typed.end();++i)if(!add(errors,i.key(),i.value()))unavailableErrors.insert(i.key());
    }
    for(const auto &name:unavailable)result.remove(name);
    for(const auto &name:unavailableErrors)errors.remove(name);
    result.insert("error_codes",errors);return result;
}

QJsonObject diagnostic(const QList<Observation> &observations, QDate from, QDate through) {
    QJsonArray computers; int ordinal = 0;
    for (const auto &observation : uniqueComputers(observations)) {
        QJsonObject row{{"computer",++ordinal},{"current",observation.current},{"agent",publicAgent(observation.agent)}};
        if (!observation.voice.isEmpty()) {
            const auto sums=speechTotals(observation.voice.value("days").toArray());
            row.insert("voice",QJsonObject{{"enabled",observation.voice.value("enabled")},{"reported_totals",sums}}); row.insert("voice_current",observation.voiceCurrent);
        }
        computers.append(row);
    }
    return {{"schema",1},{"kind","local_aggregate_diagnostics"},{"from",from.toString(Qt::ISODate)},{"through",through.toString(Qt::ISODate)},
            {"selected_routes",observations.size()},{"unavailable_routes",int(std::count_if(observations.begin(),observations.end(),[](const auto &o){return !o.current;}))},{"computers",computers}};
}
}
