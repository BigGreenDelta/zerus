#include "SniTrayIcon.h"

#include <QIcon>
#include <QMenu>

SniTrayIcon::SniTrayIcon()
{
    // main.cpp sets the display name after construction, keeping the ID hgs-tray.
    m_item.setTitle(QStringLiteral("Zerus"));
    // Без setStatus(Active) панель прячет иконку в «скрытые»: умолчание KSNI -- Passive.
    // Без setStandardActionsEnabled(false) KSNI при первом открытии дописывает в конец
    // меню свой Quit -- у TrayMenu он уже есть.
    m_item.setStandardActionsEnabled(false);
    m_item.setStatus(KStatusNotifierItem::Active);
    m_item.setIsMenu(false);
    QObject::connect(&m_item, &KStatusNotifierItem::activateRequested, &m_item,
                     [this]() { activate(); });
}

void SniTrayIcon::setIcon(const QIcon &icon)
{
    // Иконки IconFactory рисованные, без имени в теме -- поэтому пиксмапом, а не именем;
    // тултипу та же иконка. Ровно то, что делала plasma-integration для безымянного QIcon.
    m_item.setIconByPixmap(icon);
    m_item.setToolTipIconByPixmap(icon);
}

void SniTrayIcon::setToolTip(const QString &text)
{
    // У SNI-тултипа есть заголовок и подзаголовок; весь текст (в нём несколько строк --
    // см. FleetState::tooltip) идёт в заголовок, подзаголовок пуст. Так же раскладывал
    // QSystemTrayIcon::setToolTip() через plasma-integration -- вид не меняется.
    m_item.setToolTipTitle(text);
}

void SniTrayIcon::setContextMenu(QMenu *menu)
{
    // Берёт в собственность -- см. SniTrayIcon.h.
    m_item.setContextMenu(menu);
}

void SniTrayIcon::showMessage(const QString &title, const QString &body,
                              QSystemTrayIcon::MessageIcon icon, int msecs)
{
    // org.freedesktop.Notifications хочет имя иконки из темы. Имена -- те же, что даёт
    // стиль Breeze для стандартных иконок QSystemTrayIcon::showMessage(), так что балун
    // выглядит как раньше.
    QString name;
    switch (icon) {
    case QSystemTrayIcon::Information: name = QStringLiteral("dialog-information"); break;
    case QSystemTrayIcon::Warning:     name = QStringLiteral("dialog-warning"); break;
    case QSystemTrayIcon::Critical:    name = QStringLiteral("dialog-error"); break;
    case QSystemTrayIcon::NoIcon:      break;
    }
    m_item.showMessage(title, body, name, msecs);
}

void SniTrayIcon::show()
{
    // Нечего делать: KStatusNotifierItem регистрируется у StatusNotifierWatcher'а в
    // конструкторе, отдельного show() у него нет.
}
