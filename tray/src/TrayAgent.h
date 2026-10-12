#pragma once

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>

#include "AppConfig.h"
#include "DesktopPresence.h"
#include "FleetState.h"
#include "HgsClient.h"
#include "IconFactory.h"
#include "ReadMarkSync.h"
#include "TerminalBackend.h"
#include "TrayIcon.h"
#include "TrayMenu.h"
#include "AttentionTracker.h"
#include "AttentionNotifier.h"
#include "ApplicationBadge.h"

class SessionsWindow;
class SwarmWorkerSupervisor;

// Склейка: таймеры -> HgsClient -> FleetState -> иконка и меню.
// Ничего не вычисляет сам; вся логика состояния в FleetState, весь ввод-вывод в
// HgsClient, а всё, ЧТО ПОКАЗАНО в выпадающем списке, -- в TrayMenu. Здесь остаётся
// ровно то, ЧТО ПРОИСХОДИТ: сборка команды для hgs, выбор терминала, подтверждения,
// уведомления, запись выбора в tray.conf.
class TrayAgent : public QObject {
    Q_OBJECT
public:
    explicit TrayAgent(const AppConfig &cfg, QObject *parent = nullptr);
    // Zerus has no parent widget and is deleted explicitly. Само меню удаляет TrayMenu или иконка трея -- кто из них,
    // см. ~TrayMenu и порядок m_menu/m_tray ниже.
    ~TrayAgent() override;

public slots:
    void showSessions();

private slots:
    void onLocalReady(const BoxState &box);
    void onPeerReady(const BoxState &box);
    void onFailed(const QString &what, const QString &detail);
    void onMenuAboutToShow();

private:
    void refreshIcon();
    void observeAttention(const QString &host, const BoxState &box);
    void migrateReadMarks(const QString &host);
    void publishReadMarks();
    void resolveAttentionClick(bool final = false);
    // Одна строка, но своё имя: аргументы у TrayMenu::rebuild() одни и те же во всех
    // шести местах, откуда меню перестраивается, и перечислять их каждый раз -- шум.
    void rebuildMenu();
    // Клик по строке сессии (TrayMenu::sessionActivated). peer пуст -- свой бокс;
    // команда для hgs собирается ЗДЕСЬ, меню про её синтаксис не знает. tty непустой --
    // сначала пробуем поднять уже открытое окно; не вышло -- открываем терминал командой.
    void activateSession(const QString &peer, const QString &session, const QString &tty);
    void openSessionTerminal(const QString &peer, const QString &session, const QString &tty);
    void activateSessionWith(TerminalBackend &backend, const QString &peer, const QString &session, const QString &tty);
    void copyText(const QString &text);
    void launchSession(const QString &host, const QString &cmd, const QString &target, const QString &name, const QString &account, const QString &launchId);
    // Вызывать сразу после успешного backend.open(). Не про Konsole и не про D-Bus
    // -- просто спрашивает интерфейс TerminalBackend, есть ли что сказать пользователю
    // об этом конкретном открытии (см. TerminalBackend::takeAdvisory), и если да, кладёт
    // это в тот же балун, которым трей уже сообщает об ошибках открытия.
    void reportAdvisory(TerminalBackend &backend);
    // ЕДИНСТВЕННАЯ точка, из которой трей показывает уведомления. Не обёртка ради
    // обёртки: на маке QSystemTrayIcon::showMessage() теряет ПЕРВОЕ сообщение (см.
    // .cpp -- там же лог, которым это доказано), а именно первое обычно и объясняет,
    // почему клик сработал не так, как просили.
    void notify(const QString &title, const QString &body, QSystemTrayIcon::MessageIcon icon,
                int msecs);
    // Смена режима открытия из меню. Делает три вещи и ровно в этом порядке: обновляет
    // m_cfg.openMode, ПЕРЕСОЗДАЁТ бэкенд (см. .cpp -- почему пересоздание, а не сеттер)
    // и записывает выбор в tray.conf. Отметку в меню ставит сам QActionGroup внутри
    // TrayMenu, здесь её трогать не нужно.
    void setOpenMode(const QString &mode);

    AppConfig m_cfg;
    HgsClient m_client;
    FleetState m_state;
    ReadMarkSync m_readSync;
    IconFactory m_icons;
    // Терминал прячется за интерфейсом целиком: в TrayAgent нет ни строчки D-Bus, и
    // замена Konsole на что угодно другое не трогает меню.
    std::unique_ptr<TerminalBackend> m_terminal;
    // Меню -- отдельный объект, а не гора полей здесь: все грабли экспорта раскладки
    // (долгоживущие подменю, диффы их содержимого, отслеживание открытости) живут внутри
    // него, см. TrayMenu.h. Наружу оно отдаёт только намерения пользователя сигналами --
    // подписки в конструкторе.
    TrayMenu m_menu;
    // Иконка трея за интерфейсом по той же причине, что и терминал (см. TrayIcon.h:
    // почему на Linux это KStatusNotifierItem, а не QSystemTrayIcon). ПОРЯДОК ВАЖЕН:
    // объявлена ПОСЛЕ m_menu, значит, разрушается РАНЬШЕ него. На Linux иконка владеет
    // меню и удаляет его в своём деструкторе; TrayMenu держит его через QPointer и
    // должен увидеть уже nullptr, а не удалить второй раз -- см. ~TrayMenu. Переставить
    // эти два поля местами -- получить double free при выходе на Linux.
    std::unique_ptr<TrayIcon> m_tray;
    std::unique_ptr<AttentionNotifier> m_notifier;
    std::unique_ptr<ApplicationBadge> m_badge;
    AttentionTracker m_attention;
    QString m_pendingAttention;
    QTimer m_localTimer;
    QTimer m_peerTimer;
    QString m_lastError;
    QString m_localReadError;
    QSet<QString> m_pollingHosts;
    SessionsWindow *m_sessionsWindow = nullptr;
    // Records desktop activity for phone alerts; see DesktopPresence.h.
    std::unique_ptr<DesktopPresence> m_presence;
    QString m_presenceError;
#ifdef Q_OS_WIN
    // The tray runs `hgs swarm worker` itself on Windows (SwarmWorkerSupervisor.h).
    std::unique_ptr<SwarmWorkerSupervisor> m_swarmWorker;
#endif
};
