#pragma once

#include <QObject>
#include <QString>
#include <memory>

// Every platform carries an individual token; a click never uses "the last
// notification". Notifications are independent of transient tray messages.
class AttentionNotifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    // `action` labels the action button where the platform shows one; empty
    // keeps the notifier's own ("Open session").
    virtual void post(const QString &token, const QString &title, const QString &body, const QString &action = {}) = 0;
    virtual void withdraw(const QString &token) = 0;
signals:
    void activated(const QString &token, const QString &activationToken);
    void failed(const QString &token, const QString &detail);
};

std::unique_ptr<AttentionNotifier> makeAttentionNotifier();

class TrayIcon;
// Windows (WindowsAttentionNotifier.cpp): notifications are the tray icon's own
// messages, so the notifier needs that icon.
std::unique_ptr<AttentionNotifier> makeAttentionNotifier(TrayIcon &tray);
