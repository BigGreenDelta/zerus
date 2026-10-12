#pragma once

#include <QSystemTrayIcon>
#include <QMenu>
#include <QPointer>

#include "TrayIcon.h"

// QSystemTrayIcon handles drawing, notifications and mouse activation. On macOS
// attaching a menu makes AppKit open it on either button, so only Context activation
// presents the native menu (MacTrayMenu.mm). Primary activation opens the manager.
class QtTrayIcon : public TrayIcon {
public:
    QtTrayIcon();
    const QSystemTrayIcon &item() const { return m_tray; }
    void setIcon(const QIcon &icon) override;
    void setToolTip(const QString &text) override;
    void setContextMenu(QMenu *menu) override;
    void showMessage(const QString &title, const QString &body,
                     QSystemTrayIcon::MessageIcon icon, int msecs) override;
    void showClickableMessage(const QString &title, const QString &body,
                              QSystemTrayIcon::MessageIcon icon, int msecs,
                              std::function<void()> clicked) override;
    void show() override;

private:
    QSystemTrayIcon m_tray;
    QPointer<QMenu> m_menu;
    std::function<void()> m_messageClicked;   // of the latest message, see TrayIcon.h
};
