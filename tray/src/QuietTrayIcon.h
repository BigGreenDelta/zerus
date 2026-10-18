#pragma once

#include <QIcon>
#include <QMetaObject>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

#include "TrayIcon.h"

// Декоратор над TrayIcon: не дёргать иконку без нужды. Две вещи, обе -- ради мака.
//
//  1. Повтор того же самого (тот же QIcon по cacheKey, тот же текст тултипа) до
//     внутренней иконки не доходит. TrayAgent зовёт setIcon()+setToolTip() на каждом
//     локальном тике (2 с) -- так проще, чем следить, что изменилось, -- а QSystemTrayIcon
//     и cocoa-плагин Qt ничего не сравнивают: каждый вызов -- новая картинка и новый
//     тултип у NSStatusItem (qcocoasystemtrayicon.mm, updateIcon/updateToolTip). Чтобы
//     сравнение по cacheKey работало, IconFactory отдаёт на одинаковый вид один и тот же
//     QIcon (мемоизация, см. IconFactory.h), а FleetState::tooltip не меняет текст
//     каждую секунду (возраст опроса в минутах, см. formatAge в FleetState.cpp).
//  2. Пока открыто меню, иконку и тултип не трогаем вовсе; последнее отложенное
//     применяется по закрытию. Открытость -- по aboutToShow/aboutToHide меню, которое
//     сюда передали через setContextMenu().
//
// Зачем. macOS 26 держит элемент строки меню как FBScene: каждое обновление картинки --
// синхронный обмен настройками сцены с FrontBoard, а при открытом меню ещё и перерисовка
// подсвеченной кнопки. В AppKit там есть самоблокировка -- главный поток внутри callout'а
// FrontBoard заново входит в обновление настроек сцены и ждёт сам себя, стек:
// NSStatusBarButtonCell drawWithFrame -> NSSceneStatusItem _setSelectedContentFrame ->
// NSStatusItemScene updateSettings -> FBSScene _updateClientSettings ->
// BSServiceDispatchQueue performAsyncAndWait -> _dispatch_event_loop_wait_for_ownership,
// навсегда. Живой случай 2026-09-14 00:45 (macOS 26.6.1, Qt 6.11.1): трей на маке повис
// ровно так во время работы с меню и шесть дней молча висел в строке меню -- процесс жив,
// клики в никуда, в логе ничего. Та же сигнатура у чужого приложения:
// https://github.com/stablyai/orca/issues/17045 (macOS 26.5.1). Баг Apple, чинить его
// отсюда нельзя; можно не давать ему ~43 000 поводов в сутки и ни одного -- при открытом
// меню. Страховка на случай, если повиснет всё равно, -- MainThreadWatchdog.
//
// Почему не на Linux. Там aboutToHide не приходит никогда (см. TrayMenu.h про m_open):
// после первого открытия меню иконка замёрзла бы навсегда. Да и повода нет -- у
// StatusNotifierItem обновление иконки это сигнал по D-Bus, панель заберёт пиксмап, когда
// захочет. Поэтому надевается только в маковской ветке makeTrayIcon() (TrayAgent.cpp);
// сам код платформы не знает и собирается везде -- ради теста (test_quiettrayicon).
class QuietTrayIcon : public QObject, public TrayIcon {
public:
    explicit QuietTrayIcon(std::unique_ptr<TrayIcon> inner, QObject *parent = nullptr);

    void setActivationHandler(std::function<void()> handler) override;
    void setIcon(const QIcon &icon) override;
    void setToolTip(const QString &text) override;
    void setContextMenu(QMenu *menu) override;
    // Не откладываются: show() -- один раз при старте, балун -- ответ на действие
    // пользователя; картинку статус-айтема ни то ни другое не трогает.
    void showMessage(const QString &title, const QString &body,
                     QSystemTrayIcon::MessageIcon icon, int msecs) override;
    void showClickableMessage(const QString &title, const QString &body,
                              QSystemTrayIcon::MessageIcon icon, int msecs,
                              std::function<void()> clicked) override;
    void showClickableIconMessage(const QString &title, const QString &body, const QIcon &icon,
                                  int msecs, std::function<void()> clicked) override;
    void show() override;

private:
    void applyIcon(const QIcon &icon);
    void applyToolTip(const QString &text);
    // Меню закрылось (или его сменили): показать отложенное, если оно отличается от
    // показанного.
    void flushPending();

    std::unique_ptr<TrayIcon> m_inner;
    bool m_menuOpen = false;
    // Что сейчас показано во внутренней иконке -- с чем сравнивать повтор. Отдельный
    // флаг, а не «пустой QIcon»: первый setIcon() обязан дойти, каким бы он ни был.
    bool m_shownIconValid = false;
    qint64 m_shownIconKey = 0;
    std::optional<QString> m_shownToolTip;
    // Отложенное на время открытого меню -- только последнее, промежуточные никому не
    // нужны.
    std::optional<QIcon> m_pendingIcon;
    std::optional<QString> m_pendingToolTip;
    QMetaObject::Connection m_onShow;
    QMetaObject::Connection m_onHide;
};
