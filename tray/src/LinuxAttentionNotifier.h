#pragma once
#include "AttentionNotifier.h"
#include <QDBusConnection>
#include <QHash>
#include <QSet>

class LinuxAttentionNotifier : public AttentionNotifier {
    Q_OBJECT
public:
    explicit LinuxAttentionNotifier(const QDBusConnection &bus = QDBusConnection::sessionBus());
    void post(const QString &token, const QString &title, const QString &body, const QString &action = {}) override;
    void withdraw(const QString &token) override;
private slots:
    void action(uint id, const QString &action);
    void closed(uint id, uint reason);
    void activation(uint id, const QString &token);
private:
    void save();
    void close(uint id);
    QDBusConnection m_bus;
    QHash<uint, QString> m_tokens, m_activation;
    QSet<QString> m_pending;
    QString m_owner;
};
