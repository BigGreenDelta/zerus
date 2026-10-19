#pragma once

#include "HgsClient.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>

// Device-local row density, separate from shared organization and native context.
// Only collapsed rows are retained, with fixed-size identities and a hard bound.
class SessionRowPreferences {
public:
    static constexpr int MaxRows = 50000;
    static constexpr int KeyBytes = 32;
    explicit SessionRowPreferences(const QByteArray &saved = {}) {
        if (saved.size() > MaxRows * KeyBytes || saved.size() % KeyBytes) return;
        for (qsizetype i = 0; i < saved.size(); i += KeyBytes) m_collapsed.insert(saved.mid(i, KeyBytes));
    }
    static QByteArray key(const QString &host, const SessionInfo &session, bool fallback = false) {
        const QJsonArray identity = session.conversationId.isEmpty() || fallback
            ? QJsonArray{host, session.cmd, session.archiveId, session.name, session.created}
            : QJsonArray{host, session.cmd, session.archiveId, session.conversationId};
        return QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    }
    bool collapsed(const QString &host, const SessionInfo &session) const {
        return m_collapsed.contains(key(host, session));
    }
    bool observe(const QString &host, const SessionInfo &session) {
        const auto exact = key(host, session), fallback = key(host, session, true);
        if (exact == fallback || !m_collapsed.remove(fallback)) return false;
        m_collapsed.insert(exact); return true;
    }
    bool setCollapsed(const QString &host, const SessionInfo &session, bool collapsed) {
        const auto id = key(host, session);
        if (!collapsed) return m_collapsed.remove(id);
        if (m_collapsed.contains(id)) return false;
        if (m_collapsed.size() >= MaxRows) return false;
        m_collapsed.insert(id); return true;
    }
    bool empty() const { return m_collapsed.isEmpty(); }
    void expandAll() { m_collapsed.clear(); }
    QByteArray save() const {
        auto keys = m_collapsed.values(); std::sort(keys.begin(), keys.end());
        QByteArray data; data.reserve(keys.size() * KeyBytes);
        for (const auto &id : keys) data.append(id);
        return data;
    }
private:
    QSet<QByteArray> m_collapsed;
};
