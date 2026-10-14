#pragma once
#include "MachinePlatform.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QSet>

// Profiles remain machine-local launch bindings. Accounts are their shared,
// provider-reported identity; labels and native profile IDs are never identity.
namespace AccountCatalog {
inline QString profileKey(const QJsonObject &p) {
    return p["host"].toString() + QChar(0x1f) + p["id"].toString();
}
inline QString key(const QJsonObject &p) {
    const auto usage = p["usage"].toObject();
    const auto identity = usage["identity"].toObject();
    const auto email = identity["email"].toString().trimmed().toCaseFolded();
    const auto account = identity["account_id"].toString();
    if (usage["status"] == "signed_out" || (email.isEmpty() && account.isEmpty()))
        return "profile:" + profileKey(p);
    const auto scope = identity["organization"].toString().trimmed().toCaseFolded();
    const auto identityKey = p["provider"].toString() + '\n' + (account.isEmpty() ? "email:" + email : "id:" + account) + '\n' + scope;
    return "account:" + QString::fromLatin1(QCryptographicHash::hash(identityKey.toUtf8(), QCryptographicHash::Sha256).toHex());
}
// A quota belongs to the account: select a snapshot, never add quotas from
// several machines or let a failed probe replace a good snapshot.
inline bool betterUsage(const QJsonObject &usage, const QJsonObject &previous) {
    const auto quality = [](const QJsonObject &value) { return value["status"] == "ok" || value["status"] == "configured" ? (value["offline"].toBool() ? 2 : 3) : 1; };
    return quality(usage) > quality(previous)
        || (quality(usage) == quality(previous) && usage["checked_at"].toDouble() > previous["checked_at"].toDouble());
}
inline bool named(const QString &label) {
    return !label.isEmpty() && label != "Default account" && label != "Native DeepSeek account";
}
inline QJsonArray group(const QJsonArray &profiles) {
    QJsonArray groups;
    QMap<QString, int> positions;
    QMap<QString, QSet<QString>> accountIds;
    for (const auto &value : profiles) {
        auto p = value.toObject(); auto usage = p["usage"].toObject(); auto identity = usage["identity"].toObject();
        if (identity["account_id"].toString().isEmpty() || identity["email"].toString().isEmpty() || usage["status"] == "signed_out") continue;
        const auto id = key(p); identity.remove("account_id"); usage["identity"] = identity; p["usage"] = usage;
        accountIds[key(p)].insert(id);
    }
    for (const auto &value : profiles) {
        const auto p = value.toObject();
        auto id = key(p);
        // Older/native-keychain readers may provide email only. It can join a
        // known ID only when that email/scope resolves to one unique account.
        if (accountIds.value(id).size() == 1) id = *accountIds.value(id).cbegin();
        if (!positions.contains(id)) {
            positions[id] = groups.size();
            auto group = p; group["account_key"] = id; group["members"] = QJsonArray();
            group["machines"] = QJsonArray(); groups.append(group);
        }
        auto group = groups[positions[id]].toObject();
        auto members = group["members"].toArray(); members.append(p); group["members"] = members;
        auto machines = group["machines"].toArray();
        if (!machines.contains(p["machine"])) machines.append(p["machine"]);
        group["machines"] = machines;
        const auto usage = p["usage"].toObject();
        if (betterUsage(usage, group["usage"].toObject())) group["usage"] = usage;
        if (!named(group["label"].toString()) && named(p["label"].toString())) group["label"] = p["label"];
        group["installed"] = group["installed"].toBool() || p["installed"].toBool();
        groups[positions[id]] = group;
    }
    for (int i = 0; i < groups.size(); ++i) {
        auto group = groups[i].toObject();
        if (!named(group["label"].toString())) {
            const auto identity = group["usage"].toObject()["identity"].toObject();
            const auto email = identity["email"].toString();
            if (!email.isEmpty()) group["label"] = email;
        }
        groups[i] = group;
    }
    return groups;
}
inline bool contains(const QJsonObject &account, const QString &profile) {
    for (const auto &member : account["members"].toArray())
        if (profileKey(member.toObject()) == profile) return true;
    return false;
}
// Whether a machine of this style starts the provider's agent. The local Windows
// computer runs neither DeepSeek (a Phase 2 non-goal) nor OpenCode, whose launch
// path does not resolve npm .cmd shims there (spec D16).
inline bool launchable(const QString &provider, MachinePlatform::Style style) {
    return style == MachinePlatform::Style::Posix || (provider != "dsh" && provider != "opencode");
}
}
