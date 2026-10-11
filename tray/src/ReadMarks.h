#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QSettings>
#include <QString>

// One read mark for a session conversation, written to the session's own host
// with `hgs [@host] read`. The host applies a field only while it is still the
// session's current value, so a late mark never hides a newer reply or request.
struct ReadMark {
    QString host, name, runId, conversationId, replyId, attentionSignature;

    QString key() const {
        return QString::fromUtf8(QJsonDocument(QJsonArray{host, name, conversationId}).toJson(QJsonDocument::Compact));
    }
    bool isEmpty() const { return replyId.isEmpty() && attentionSignature.isEmpty(); }
    bool operator==(const ReadMark &other) const {
        return host == other.host && name == other.name && runId == other.runId && conversationId == other.conversationId
            && replyId == other.replyId && attentionSignature == other.attentionSignature;
    }
    QJsonObject toJson() const {
        return {{"host", host}, {"name", name}, {"run_id", runId}, {"conversation_id", conversationId},
                {"reply_id", replyId}, {"attention_signature", attentionSignature}};
    }
    static ReadMark fromJson(const QJsonObject &value) {
        return {value.value("host").toString(), value.value("name").toString(), value.value("run_id").toString(),
                value.value("conversation_id").toString(), value.value("reply_id").toString(),
                value.value("attention_signature").toString()};
    }
    // C1 stdin; a field that is not being marked is null, which hosts never apply.
    QJsonObject payload() const {
        const auto nullable = [](const QString &value) { return value.isEmpty() ? QJsonValue() : QJsonValue(value); };
        return {{"run_id", runId}, {"conversation_id", conversationId},
                {"reply_id", nullable(replyId)}, {"attention_signature", nullable(attentionSignature)}};
    }
    // A newer mark for the same run keeps the field it does not set.
    static ReadMark merged(const ReadMark &old, const ReadMark &next) {
        if (old.runId != next.runId || old.conversationId != next.conversationId) return next;
        auto result = next;
        if (result.replyId.isEmpty()) result.replyId = old.replyId;
        if (result.attentionSignature.isEmpty()) result.attentionSignature = old.attentionSignature;
        return result;
    }
};

// Marks not yet shown by their hosts' snapshots. Kept in QSettings so a GUI
// restart keeps retrying; the root agent and the workspace window share it.
namespace ReadMarkStore {
inline QJsonObject load() {
    return QJsonDocument::fromJson(QSettings().value("attention/pendingReads").toByteArray()).object();
}
inline void save(const QJsonObject &marks) {
    QSettings().setValue("attention/pendingReads", QJsonDocument(marks).toJson(QJsonDocument::Compact));
}
inline void add(const QList<ReadMark> &marks) {
    if (marks.isEmpty()) return;
    auto stored = load();
    for (const auto &mark : marks)
        if (!mark.isEmpty())
            stored[mark.key()] = ReadMark::merged(ReadMark::fromJson(stored.value(mark.key()).toObject()), mark).toJson();
    save(stored);
}
// Only entries still equal to the settled mark: a newer merged mark survives.
inline void remove(const QList<ReadMark> &marks) {
    if (marks.isEmpty()) return;
    auto stored = load();
    for (const auto &mark : marks)
        if (ReadMark::fromJson(stored.value(mark.key()).toObject()) == mark) stored.remove(mark.key());
    save(stored);
}
inline QJsonArray migratedHosts() {
    return QJsonDocument::fromJson(QSettings().value("attention/sharedReadMigrated").toByteArray()).array();
}
inline bool migrated(const QString &host) { return migratedHosts().contains(host); }
inline void setMigrated(const QString &host) {
    auto hosts = migratedHosts();
    if (!hosts.contains(host)) hosts.append(host);
    QSettings().setValue("attention/sharedReadMigrated", QJsonDocument(hosts).toJson(QJsonDocument::Compact));
}
} // namespace ReadMarkStore
