#include "QuietTrayIcon.h"

#include <QMenu>

QuietTrayIcon::QuietTrayIcon(std::unique_ptr<TrayIcon> inner, QObject *parent)
    : QObject(parent)
    , m_inner(std::move(inner))
{
}

void QuietTrayIcon::setActivationHandler(std::function<void()> handler)
{
    m_inner->setActivationHandler(std::move(handler));
}

void QuietTrayIcon::setIcon(const QIcon &icon)
{
    if (m_menuOpen) {
        m_pendingIcon = icon;
        return;
    }
    applyIcon(icon);
}

void QuietTrayIcon::setToolTip(const QString &text)
{
    if (m_menuOpen) {
        m_pendingToolTip = text;
        return;
    }
    applyToolTip(text);
}

void QuietTrayIcon::setContextMenu(QMenu *menu)
{
    // Старое меню больше ничего не гейтит: если оно было открыто, считаем закрытым и
    // отдаём отложенное -- aboutToHide от него уже не придёт.
    QObject::disconnect(m_onShow);
    QObject::disconnect(m_onHide);
    m_menuOpen = false;
    m_inner->setContextMenu(menu);
    if (menu) {
        m_onShow = connect(menu, &QMenu::aboutToShow, this, [this]() { m_menuOpen = true; });
        m_onHide = connect(menu, &QMenu::aboutToHide, this, [this]() {
            m_menuOpen = false;
            flushPending();
        });
    }
    flushPending();
}

void QuietTrayIcon::showMessage(const QString &title, const QString &body,
                                QSystemTrayIcon::MessageIcon icon, int msecs)
{
    m_inner->showMessage(title, body, icon, msecs);
}

void QuietTrayIcon::showClickableMessage(const QString &title, const QString &body,
                                         QSystemTrayIcon::MessageIcon icon, int msecs,
                                         std::function<void()> clicked)
{
    m_inner->showClickableMessage(title, body, icon, msecs, std::move(clicked));
}

void QuietTrayIcon::showClickableIconMessage(const QString &title, const QString &body,
                                             const QIcon &icon, int msecs,
                                             std::function<void()> clicked)
{
    m_inner->showClickableIconMessage(title, body, icon, msecs, std::move(clicked));
}

void QuietTrayIcon::show()
{
    m_inner->show();
}

void QuietTrayIcon::applyIcon(const QIcon &icon)
{
    if (m_shownIconValid && icon.cacheKey() == m_shownIconKey)
        return;
    m_inner->setIcon(icon);
    m_shownIconValid = true;
    m_shownIconKey = icon.cacheKey();
}

void QuietTrayIcon::applyToolTip(const QString &text)
{
    if (m_shownToolTip == text)
        return;
    m_inner->setToolTip(text);
    m_shownToolTip = text;
}

void QuietTrayIcon::flushPending()
{
    if (m_pendingIcon) {
        applyIcon(*m_pendingIcon);
        m_pendingIcon.reset();
    }
    if (m_pendingToolTip) {
        applyToolTip(*m_pendingToolTip);
        m_pendingToolTip.reset();
    }
}
