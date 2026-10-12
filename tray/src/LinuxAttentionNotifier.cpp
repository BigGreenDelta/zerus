#include "LinuxAttentionNotifier.h"
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

namespace {
const QString service = QStringLiteral("org.freedesktop.Notifications");
QDBusMessage call(const QString &method) {
    return QDBusMessage::createMethodCall(service, "/org/freedesktop/Notifications", service, method);
}
}

LinuxAttentionNotifier::LinuxAttentionNotifier(const QDBusConnection &bus) : m_bus(bus)
{
    if (m_bus.interface()) m_owner = m_bus.interface()->serviceOwner(service).value();
    QSettings settings;
    if (!m_owner.isEmpty() && settings.value("attention/linuxOwner").toString() == m_owner) {
        const auto saved = QJsonDocument::fromJson(settings.value("attention/linuxTokens").toByteArray()).object();
        for (auto it = saved.begin(); it != saved.end(); ++it) m_tokens[it.key().toUInt()] = it.value().toString();
    }
    m_bus.connect(service, "/org/freedesktop/Notifications", service, "ActionInvoked", this, SLOT(action(uint,QString)));
    m_bus.connect(service, "/org/freedesktop/Notifications", service, "NotificationClosed", this, SLOT(closed(uint,uint)));
    m_bus.connect(service, "/org/freedesktop/Notifications", service, "ActivationToken", this, SLOT(activation(uint,QString)));
    auto *watcher = new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this](const QString &, const QString &, const QString &owner) {
        m_owner = owner; m_tokens.clear(); m_activation.clear(); save();
    });
}

void LinuxAttentionNotifier::save()
{
    QJsonObject tokens;
    for (auto it = m_tokens.begin(); it != m_tokens.end(); ++it) tokens[QString::number(it.key())] = it.value();
    QSettings settings; settings.setValue("attention/linuxOwner", m_owner);
    settings.setValue("attention/linuxTokens", QJsonDocument(tokens).toJson(QJsonDocument::Compact));
}

void LinuxAttentionNotifier::post(const QString &token, const QString &title, const QString &body, const QString &action)
{
    if (m_pending.contains(token) || m_tokens.values().contains(token)) return;
    m_pending.insert(token);
    auto message = call("Notify");
    const QVariantMap hints{{"desktop-entry", "hgs-tray"}, {"urgency", QVariant::fromValue(uchar(1))},
        {"category", "im.received"}};
    const QString label = action.isEmpty() ? tr("Open session") : action;
    message.setArguments({"Zerus", uint(0), "hgs-zerus-swarm-symbolic", title,
        body.toHtmlEscaped(), QStringList{"default", label, "open-session", label}, hints, -1});
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, token](QDBusPendingCallWatcher *watcher) {
        const QDBusPendingReply<uint> reply = *watcher; watcher->deleteLater();
        const bool wanted = m_pending.remove(token);
        if (reply.isError()) { if (wanted) emit failed(token, reply.error().message()); return; }
        if (!wanted) { close(reply.value()); return; }
        m_tokens[reply.value()] = token; save();
    });
}

void LinuxAttentionNotifier::close(uint id)
{
    auto message = call("CloseNotification"); message.setArguments({id}); m_bus.asyncCall(message);
}

void LinuxAttentionNotifier::withdraw(const QString &token)
{
    m_pending.remove(token);
    for (uint id : m_tokens.keys(token)) { m_tokens.remove(id); m_activation.remove(id); close(id); }
    save();
}

void LinuxAttentionNotifier::action(uint id, const QString &action)
{
    if ((action != "default" && action != "open-session") || !m_tokens.contains(id)) return;
    emit activated(m_tokens.value(id), m_activation.take(id));
}
void LinuxAttentionNotifier::activation(uint id, const QString &token)
{
    if (m_tokens.contains(id)) m_activation[id] = token;
}
void LinuxAttentionNotifier::closed(uint id, uint reason)
{
    // Expired Plasma banners can remain actionable in notification history.
    if (reason == 1) return;
    m_tokens.remove(id); m_activation.remove(id); save();
}
std::unique_ptr<AttentionNotifier> makeAttentionNotifier() { return std::make_unique<LinuxAttentionNotifier>(); }
