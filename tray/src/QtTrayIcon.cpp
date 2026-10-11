#include "QtTrayIcon.h"

#include <QIcon>
#include <QMenu>

#ifdef Q_OS_MACOS
#include "MacTrayMenu.h"
#endif

QtTrayIcon::QtTrayIcon()
{
    QObject::connect(&m_tray, &QSystemTrayIcon::activated, &m_tray,
                     [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            activate();
#ifdef Q_OS_MACOS
        else if (reason == QSystemTrayIcon::Context && m_menu)
            showMacTrayMenu(m_menu);
#endif
    });
    QObject::connect(&m_tray, &QSystemTrayIcon::messageClicked, &m_tray, [this]() {
        if (const auto clicked = m_messageClicked)
            clicked();
    });
}

void QtTrayIcon::setIcon(const QIcon &icon)
{
    m_tray.setIcon(icon);
}

void QtTrayIcon::setToolTip(const QString &text)
{
    m_tray.setToolTip(text);
}

void QtTrayIcon::setContextMenu(QMenu *menu)
{
    // В собственность не берёт (документация QSystemTrayIcon) -- владелец остаётся
    // TrayMenu, см. ~TrayMenu.
    m_menu = menu;
#ifndef Q_OS_MACOS
    m_tray.setContextMenu(menu);
#endif
}

void QtTrayIcon::showMessage(const QString &title, const QString &body,
                             QSystemTrayIcon::MessageIcon icon, int msecs)
{
    m_messageClicked = {};
    m_tray.showMessage(title, body, icon, msecs);
}

void QtTrayIcon::showClickableMessage(const QString &title, const QString &body,
                                      QSystemTrayIcon::MessageIcon icon, int msecs,
                                      std::function<void()> clicked)
{
    m_messageClicked = std::move(clicked);
    m_tray.showMessage(title, body, icon, msecs);
}

void QtTrayIcon::show()
{
    m_tray.show();
}
