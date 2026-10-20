#pragma once

#include <QList>
#include <QMap>
#include <QString>

#include "HgsClient.h"
#include "ReadMarks.h"

// Состояние флота: свой бокс плюс пиры, каждый со временем последнего успешного опроса.
//
// Правило счёта: спящий ноут — это ПАУЗА, а не смерть. Его сессии живы и продолжают
// считаться; несвежесть данных показывается точкой у цифры, а не вычитанием из неё.
// Врать в меньшую сторону так же плохо, как в большую.
//
// Это распространяется и на переход ok:true -> ok:false: когда пир засыпает, hgs
// возвращает ok:false с пустым sessions (спросить уже некого), но FleetState не даёт
// этому затереть последний известный список — иначе цифра просела бы ровно в момент,
// когда крышку ноута закрыли, что и есть запрещённая ложь вниз. См. FleetState.cpp.
class FleetState {
public:
    // static const qint64 в заголовке не ODR-используется большинством выражений, но
    // QCOMPARE/QVERIFY в тестах берут его по ссылке — без определения это падает на
    // линковке. constexpr в C++17 неявно inline для static-членов класса, поэтому
    // определение есть в каждой единице трансляции и линковщик не жалуется. Простой
    // static const этого не даёт (нужно было бы отдельное определение в .cpp).
    static constexpr qint64 kPeerStaleMs = 180000;   // 3 минуты без ответа -> данные несвежие

    void setLocal(const BoxState &box, qint64 nowMs);
    void registerPeer(const QString &alias);     // объявить пир до первого ответа
    void setPeer(const BoxState &box, qint64 nowMs);

    void setReadReplies(const QJsonObject &read);
    QJsonObject readReplies() const { return m_readReplies; }
    // Replies Mark all as read may mark: unread, except in sessions marked as needing
    // attention. markRepliesRead checks both again when it applies a snapshot.
    QJsonObject unreadReplies() const;
    int markRepliesRead(const QJsonObject &replies);
    bool markReplyRead(const QString &host, const QString &name, const QString &conversation, const QString &reply);
    void setAttentionMarks(const QJsonObject &marks);
    QJsonObject attentionMarks() const { return m_attentionMarks; }
    bool markSessionRead(const QString &host, const SessionInfo &expected);
    bool setReviewLater(const QString &host, const SessionInfo &expected, bool marked);
    bool applyNotificationPreferences(const QString &host, const SessionInfo &expected, const QJsonObject &preferences);
    // Shared read state. Hosts with `hgs read` own unread/acknowledged state;
    // marks this GUI queued but no snapshot shows yet keep the UI read.
    void setPendingReads(const QJsonObject &pending);
    QJsonObject pendingReads() const { return m_pendingReads; }
    QList<ReadMark> takeNewReadMarks();       // queued by mark*Read since the last call
    QList<ReadMark> takeSettledReadMarks();   // shown by a snapshot or no longer current
    QList<ReadMark> legacyReadMarks(const QString &host) const;   // one-time QSettings migration
    bool supportsSharedRead(const QString &host) const;
    int totalSessions() const;
    int attentionSessions(qint64 nowMs) const;
    bool isStale(qint64 nowMs) const;            // хоть один объявленный пир протух
    QString tooltip(qint64 nowMs) const;         // "4 here, 2 on mac (20s ago)"

    const BoxState &local() const { return m_local; }
    QList<QString> peerNames() const;
    const BoxState *peer(const QString &alias) const;
    qint64 peerPolledAt(const QString &alias) const;

private:
    void updateUnread(const QString &host, BoxState &box);
    void queueRead(const QString &host, const SessionInfo &s, bool reply, bool attention);
    QJsonObject m_pendingReads;
    QList<ReadMark> m_newReads, m_settledReads;
    QJsonObject m_readReplies;
    QJsonObject m_attentionMarks;
    SessionInfo *matchingSession(const QString &host, const SessionInfo &expected);
    struct PeerEntry { BoxState box; qint64 polledAt = 0; bool everPolled = false; };
    BoxState m_local;
    bool m_haveLocal = false;
    QMap<QString, PeerEntry> m_peers;
};
