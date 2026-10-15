#include "SessionGraphStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <cmath>

namespace {
constexpr qsizetype MaxBytes = 32 * 1024 * 1024;
constexpr qsizetype MaxItems = 20000;
bool uuid(const QString &value) {
    const QUuid id(value);
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == value;
}
bool number(const QJsonValue &value, qint64 *result) {
    const double n = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(n) || n < 0 || n > 9007199254740991. ||
        std::floor(n) != n)
        return false;
    *result = qint64(n);
    return true;
}
bool itemId(const QString &value) {
    static const QRegularExpression pattern(
        "^(node|child|binding|relation|alias|event):[0-9a-f]{64}$");
    return pattern.match(value).hasMatch();
}
bool item(const QJsonObject &value) {
    static const QStringList kinds{"node",  "child", "binding", "relation",
                                   "alias", "event", "conflict"};
    return kinds.contains(value["kind"].toString()) && value["value"].isObject() &&
           QJsonDocument(value).toJson(QJsonDocument::Compact).size() <= 256 * 1024;
}
bool bounded(const QJsonObject &items) {
    return items.size() <= MaxItems &&
           QJsonDocument(items).toJson(QJsonDocument::Compact).size() <= MaxBytes;
}
bool token(const QJsonValue &value) {
    return value.isString() && !value.toString().isEmpty() && value.toString().size() <= 2048;
}
} // namespace

const SessionGraphStore::Host *SessionGraphStore::host(const QString &route) const {
    const auto found = m_hosts.constFind(route);
    return found == m_hosts.cend() ? nullptr : &found.value();
}
QStringList SessionGraphStore::routes() const {
    auto result = m_hosts.keys();
    result.sort();
    return result;
}
QString SessionGraphStore::nextCursor(const QString &route) const {
    const auto pending = m_pending.constFind(route);
    return pending == m_pending.cend() ? (host(route) ? host(route)->cursor : QString())
                                       : pending->host.cursor;
}
void SessionGraphStore::forget(const QString &route) {
    m_pending.remove(route);
    m_hosts.remove(route);
}
void SessionGraphStore::discardPending(const QString &route) { m_pending.remove(route); }

SessionGraphStore::Result SessionGraphStore::apply(const QString &route, const QJsonObject &page,
                                                   bool changes, const QString &requested,
                                                   qint64 now) {
    qint64 revision = 0;
    const QString machine = page["machine_id"].toString(), epoch = page["epoch"].toString();
    if ((!m_hosts.contains(route) && !m_pending.contains(route) &&
         m_hosts.size() + m_pending.size() >= 128) ||
        route.size() > 256 || now < 0 || page["schema"] != 1 || !uuid(machine) || !uuid(epoch) ||
        !number(page["revision"], &revision) || !page["reset_required"].isBool() ||
        !page["coverage"].isObject() ||
        QJsonDocument(page).toJson(QJsonDocument::Compact).size() > 1024 * 1024)
        return Rejected;
    if (page["reset_required"].toBool()) {
        m_pending.remove(route);
        return ResetRequired;
    }
    if (!page["items"].isArray() || !page["has_more"].isBool() || !token(page["cursor"]))
        return Rejected;
    const auto rows = page["items"].toArray();
    const bool more = page["has_more"].toBool();
    const auto cursor = page["cursor"].toString();
    if (rows.size() > 200 || (more && (rows.isEmpty() || cursor == requested)))
        return Rejected;
    Host candidate;
    QString afterId;
    if (changes) {
        const auto *previous = host(route);
        if (!previous || requested.isEmpty() || requested != previous->cursor ||
            previous->machine != machine || previous->epoch != epoch)
            return ResetRequired;
        if (revision < previous->revision)
            return Rejected;
        candidate = *previous;
    } else if (!requested.isEmpty()) {
        const auto stage = m_pending.constFind(route);
        if (stage == m_pending.cend() || requested != stage->host.cursor ||
            machine != stage->host.machine || epoch != stage->host.epoch ||
            revision != stage->host.revision)
            return Rejected;
        candidate = stage->host;
        afterId = stage->afterId;
    } else {
        candidate.machine = machine;
        candidate.epoch = epoch;
        candidate.revision = revision;
    }
    qint64 consumed = candidate.revision;
    QSet<QString> seen;
    for (const auto &value : rows) {
        if (!value.isObject())
            return Rejected;
        const auto row = value.toObject();
        const auto id = row["id"].toString();
        qint64 rev = 0;
        if (!itemId(id) || seen.contains(id) || !number(row["revision"], &rev) || rev > revision ||
            !row["deleted"].isBool() || !row["item"].isObject() || !item(row["item"].toObject()))
            return Rejected;
        if (changes) {
            if (rev <= consumed)
                return Rejected;
            consumed = rev;
        } else {
            if (row["deleted"].toBool() || id <= afterId || candidate.items.contains(id))
                return Rejected;
            afterId = id;
        }
        seen.insert(id);
        if (row["deleted"].toBool())
            candidate.items.remove(id);
        else
            candidate.items[id] = row["item"];
    }
    if (!bounded(candidate.items))
        return Rejected;
    candidate.cursor = cursor;
    candidate.coverage = page["coverage"].toObject();
    candidate.sampledAt = now;
    candidate.revision = changes && more ? consumed : revision;
    // Bound the complete in-memory/cache projection, not only a host response.
    qsizetype total = QJsonDocument(candidate.items).toJson(QJsonDocument::Compact).size();
    for (auto it = m_hosts.cbegin(); it != m_hosts.cend(); ++it)
        if (it.key() != route)
            total += QJsonDocument(it->items).toJson(QJsonDocument::Compact).size();
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it)
        if (it.key() != route)
            total += QJsonDocument(it->host.items).toJson(QJsonDocument::Compact).size();
    if (total > MaxBytes)
        return Rejected;
    if (!changes && more) {
        m_pending[route] = {candidate, afterId};
        return More;
    }
    m_pending.remove(route);
    m_hosts[route] = candidate;
    return more ? More : Applied;
}

QJsonObject SessionGraphStore::cache() const {
    QJsonObject hosts;
    for (auto it = m_hosts.cbegin(); it != m_hosts.cend(); ++it)
        hosts[it.key()] = QJsonObject{{"machine", it->machine},      {"epoch", it->epoch},
                                      {"cursor", it->cursor},        {"revision", it->revision},
                                      {"sampled_at", it->sampledAt}, {"items", it->items},
                                      {"coverage", it->coverage}};
    return {{"schema", 1}, {"hosts", hosts}};
}
bool SessionGraphStore::restore(const QJsonObject &data) {
    if (data["schema"] != 1 || !data["hosts"].isObject() || data["hosts"].toObject().size() > 128 ||
        QJsonDocument(data).toJson(QJsonDocument::Compact).size() > MaxBytes)
        return false;
    QHash<QString, Host> hosts;
    const auto values = data["hosts"].toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto v = it.value().toObject();
        Host h;
        h.machine = v["machine"].toString();
        h.epoch = v["epoch"].toString();
        h.cursor = v["cursor"].toString();
        h.items = v["items"].toObject();
        h.coverage = v["coverage"].toObject();
        if (it.key().size() > 256 || !uuid(h.machine) || !uuid(h.epoch) || !token(v["cursor"]) ||
            !number(v["revision"], &h.revision) || !number(v["sampled_at"], &h.sampledAt) ||
            !v["items"].isObject() || !v["coverage"].isObject() || !bounded(h.items))
            return false;
        for (auto entry = h.items.begin(); entry != h.items.end(); ++entry)
            if (!itemId(entry.key()) || !entry.value().isObject() ||
                !item(entry.value().toObject()))
                return false;
        hosts[it.key()] = h;
    }
    m_pending.clear();
    m_hosts = hosts;
    return true;
}
