#pragma once

#include <functional>
#include <utility>

#include <QString>
#include <QSystemTrayIcon>   // MessageIcon -- общий словарь для notify(), см. showMessage()

class QIcon;
class QMenu;

// Platform tray adapter: primary click opens Zerus; the context menu stays
// native to the desktop. SniTrayIcon owns its QMenu, QtTrayIcon does not.
class TrayIcon {
public:
    virtual ~TrayIcon() = default;
    virtual void setActivationHandler(std::function<void()> handler) {
        m_activationHandler = std::move(handler);
    }
    virtual void setIcon(const QIcon &icon) = 0;
    virtual void setToolTip(const QString &text) = 0;
    // Меню показывает система/панель по клику. КТО ИМ ВЛАДЕЕТ -- зависит от реализации:
    // QtTrayIcon в собственность не берёт (как и QSystemTrayIcon), SniTrayIcon -- берёт
    // (так устроен KStatusNotifierItem, и обойти это нельзя). TrayMenu про это знает: его
    // m_menu -- QPointer, см. ~TrayMenu и порядок членов в TrayAgent.h.
    virtual void setContextMenu(QMenu *menu) = 0;
    // Балун/уведомление. Словарь иконок -- QSystemTrayIcon::MessageIcon на обеих
    // платформах: SniTrayIcon переводит его в имена тем (dialog-warning и т.п.).
    virtual void showMessage(const QString &title, const QString &body,
                             QSystemTrayIcon::MessageIcon icon, int msecs) = 0;
    // As showMessage(); clicked runs when the user clicks this message. A tray icon
    // shows one message at a time and QSystemTrayIcon reports a click without
    // saying on which message, so a later message replaces the callback. Icons
    // without click support only show the message.
    virtual void showClickableMessage(const QString &title, const QString &body,
                                      QSystemTrayIcon::MessageIcon icon, int msecs,
                                      std::function<void()> clicked) {
        (void)clicked;
        showMessage(title, body, icon, msecs);
    }
    // Показать иконку. Звать после setIcon(): у QSystemTrayIcon пустая иконка видна как
    // дырка в панели. У SniTrayIcon это no-op -- StatusNotifierItem регистрируется у
    // панели уже в конструкторе.
    virtual void show() = 0;

protected:
    void activate() { if (m_activationHandler) m_activationHandler(); }

private:
    std::function<void()> m_activationHandler;
};
