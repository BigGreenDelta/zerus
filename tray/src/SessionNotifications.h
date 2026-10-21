#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <cmath>

// Host-confirmed conversation preferences. These are not read/approval marks.
namespace SessionNotifications {
inline bool keys(const QJsonObject &object, const QStringList &expected) {
    const auto actual = object.keys();
    return QSet<QString>(actual.cbegin(), actual.cend()) == QSet<QString>(expected.cbegin(), expected.cend());
}
inline bool text(const QJsonValue &value, int maximum, bool nonempty = false, bool lineFeed = false) {
    if (!value.isString()) return false;
    const auto string = value.toString();
    if ((nonempty && string.isEmpty()) || string.toUtf8().size() > maximum) return false;
    for (const auto character : string)
        if ((character.unicode() < 32 && !(lineFeed && character == QChar('\n'))) || character.unicode() == 127) return false;
    return true;
}
inline bool identity(const QJsonValue &value, int maximum) {
    return text(value, maximum, true);
}
inline qint64 revision(const QJsonValue &value) {
    if (!value.isDouble()) return -1;
    const auto number = value.toInteger(-1);
    return number >= 0 && value.toDouble() == double(number) ? number : -1;
}
inline bool started(const QJsonValue &value) {
    return value.isNull() || (value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0);
}
inline bool attention(const QJsonObject &value, bool main) {
    if (!keys(value, main ? QStringList{"run_id", "phase", "request_id", "started_at"}
                          : QStringList{"phase", "request_id", "started_at"})) return false;
    if (main && !identity(value.value("run_id"), 128)) return false;
    if (!text(value.value("phase"), 256) || !text(value.value("request_id"), 65536, false, true) || !started(value.value("started_at"))) return false;
    if (value.value("request_id").toString().isEmpty() && value.value("started_at").isNull()) return false;
    return main || QStringList{"approval", "input", "attention"}.contains(value.value("phase").toString());
}
inline bool valid(const QJsonObject &value, const QString &conversation) {
    // The host applies this same ASCII-escaped budget before persisting unmute.
    qsizetype asciiBytes = 0;
    const auto json = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
    for (const auto character : json) {
        asciiBytes += character.unicode() < 128 ? 1 : 6;
        if (asciiBytes > 384 * 1024) return false;
    }
    if (!keys(value, {"conversation_id", "muted", "revision", "suppressed"})
        || !identity(value.value("conversation_id"), 256) || value.value("conversation_id").toString() != conversation
        || !value.value("muted").isBool() || revision(value.value("revision")) < 0 || !value.value("suppressed").isObject()) return false;
    const auto baseline = value.value("suppressed").toObject();
    if (!keys(baseline, {"reply_id", "attention_signature", "optional_question_ids", "main_attention", "child_attention"})) return false;
    const auto reply = baseline.value("reply_id"), signature = baseline.value("attention_signature");
    if (!reply.isNull() && !identity(reply, 256)) return false;
    static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
    if (!signature.isNull() && (!signature.isString() || !sha.match(signature.toString()).hasMatch())) return false;
    if (!baseline.value("optional_question_ids").isArray() || !baseline.value("child_attention").isObject()) return false;
    const auto optional = baseline.value("optional_question_ids").toArray();
    if (optional.size() > 64) return false;
    QByteArray previous; bool first = true;
    for (const auto &id : optional) {
        const auto bytes = id.toString().toUtf8();
        if (!identity(id, 256) || (!first && bytes <= previous)) return false;
        previous = bytes; first = false;
    }
    const auto main = baseline.value("main_attention");
    if (!main.isNull() && (!main.isObject() || !attention(main.toObject(), true))) return false;
    const auto children = baseline.value("child_attention").toObject();
    if (children.size() > 256) return false;
    for (auto it = children.begin(); it != children.end(); ++it)
        if (!identity(it.key(), 256) || !it.value().isObject() || !attention(it.value().toObject(), false)) return false;
    return true;
}
inline QJsonValue positive(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() > 0 ? value : QJsonValue(QJsonValue::Null);
}
inline bool matches(const QJsonObject &saved, const QString &phase, const QString &request, const QJsonValue &at) {
    return saved.value("phase").toString() == phase && saved.value("request_id").toString() == request
        && (!request.isEmpty() || (!positive(at).isNull() && saved.value("started_at") == positive(at)));
}
inline QString childPhase(const QJsonObject &child) {
    const auto display = child.value("display_state").toString();
    return display.isEmpty() ? child.value("state").toString() : display;
}
inline QString childRequest(const QJsonObject &child) {
    const auto request = child.value("attention_id").toString();
    return request.isEmpty() ? child.value("question_id").toString() : request;
}
inline QJsonValue childStarted(const QJsonObject &child) {
    const auto at = positive(child.value("attention_started"));
    return at.isNull() ? positive(child.value("last_event_at")) : at;
}
}
