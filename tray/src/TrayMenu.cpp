#include "TrayMenu.h"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QMenu>

#include "FleetState.h"
#include "UpdateController.h"

namespace {

// QAction::setText() трактует одиночный '&' как мнемонику (подчёркивает следующую
// букву и съедает сам символ). Имена сессий -- это "cmd/project[/tag]", а project
// берётся из имени каталога проекта: каталог вида "R&D" или "AT&T" сделает амперсанд
// достижимым безо всякой экзотики. "&&" -- это то, как Qt пишет литеральный '&'.
QString escapeAmp(QString s)
{
    return s.replace(QLatin1Char('&'), QStringLiteral("&&"));
}

QString sessionLabel(const SessionInfo &s)
{
    QString label = escapeAmp(s.name);
    if (s.state == QLatin1String("paused"))
        return label + TrayMenu::tr("  (paused)");
    if (s.state == QLatin1String("stopped"))
        return label + TrayMenu::tr("  (stopped)");
    return s.attached > 0 ? QStringLiteral("● ") + label : label;
}

// Пункт, который нечем нажать: заголовок бокса, «no sessions», «polling…».
void addDisabled(QMenu *menu, const QString &text)
{
    QAction *a = menu->addAction(text);
    a->setEnabled(false);
}

// ЭТО ОГРАНИЧЕНИЕ -- LINUX/PLASMA, НЕ УНИВЕРСАЛЬНОЕ. QMenu::addSection() рисует красивый
// заголовок в родном (in-process) меню, но на Linux трей живёт не в процессе:
// StatusNotifierItem отдаёт это же QMenu наружу через протокол DBusMenu, и именно так его
// показывает панель Plasma. Проверено вручную через
// `busctl --user call ... com.canonical.dbusmenu GetLayout` на живом меню: у пункта,
// созданного addSection(), после экспорта остаётся только "type":"separator" -- сам
// текст секции пропадает целиком, никакого свойства "label" на той стороне нет. Поэтому
// заголовок бокса здесь -- обычный disabled QAction (label доезжает без потерь), а не
// addSection(); разделитель между блоками -- addSeparator(), он этот путь переживает.
//
// На маке экспорта нет вовсе: меню строит Cocoa (нативный NSMenu) прямо в процессе, и
// QtDBus в маковую сборку даже не линкуется (CMakeLists.txt: Qt6::DBus только if(LINUX)).
// Так что маковому читателю искать здесь свою беду не надо -- addSection() там, скорее
// всего, отрисовался бы штатно. Обход при этом безвреден на обеих платформах: disabled
// QAction -- это просто серый пункт, поэтому #ifdef тут не заводится.
void addBoxHeader(QMenu *menu, const QString &label, bool leadingSeparator)
{
    if (leadingSeparator)
        menu->addSeparator();
    addDisabled(menu, label);
}

// Подпись пункта «Открывать в ▸ …» для машинного значения режима. Собственного списка
// режимов здесь НЕТ -- какие они бывают, знает AppConfig::openModes() (и передаёт их
// сюда конструктором), а это только перевод на человеческий; незнакомое значение
// показывается как есть, а не прячется (пункт без подписи выглядел бы как сломанное
// меню).
// windowsTerminal: the Windows set, where tab and window mean Windows Terminal.
QString openModeLabel(const QString &mode, bool windowsTerminal)
{
    if (mode == QLatin1String("auto"))
        return TrayMenu::tr("Automatic");
    if (mode == QLatin1String("tab"))
        return windowsTerminal ? TrayMenu::tr("Windows Terminal tab") : TrayMenu::tr("Tab");
    if (mode == QLatin1String("window"))
        return windowsTerminal ? TrayMenu::tr("Windows Terminal window") : TrayMenu::tr("Window");
    if (mode == QLatin1String("console"))
        return TrayMenu::tr("Console window");
    if (mode == QLatin1String("terminal"))
        return TrayMenu::tr("Terminal.app");
    if (mode == QLatin1String("clipboard"))
        return TrayMenu::tr("Clipboard");
    return mode;
}

} // namespace

TrayMenu::TrayMenu(const QStringList &openModes, const QString &currentMode,
                   bool terminalTemplateSet, QObject *parent)
    : QObject(parent)
    , m_menu(new QMenu())
    , m_openModeMenu(new QMenu(tr("Open in"), m_menu))
{
    // Флаг выставляется ДО ретрансляции: подписчик почти наверняка позовёт из своего
    // обработчика rebuild(), и следующий фоновый тик у него же должен уже видеть
    // isOpen()==true и не перестраивать список под курсором, пока пользователь читает.
    connect(m_menu, &QMenu::aboutToShow, this, [this]() {
        m_open = true;
        emit aboutToShow();
    });
    // aboutToHide -- вторая половина источника правды для m_open (см. TrayMenu.h):
    // как только выпадающий список закрылся, снова можно перестраивать меню в фоне.
    connect(m_menu, &QMenu::aboutToHide, this, [this]() { m_open = false; });

    // Modes come from AppConfig so offered values and accepted config stay in sync.
    auto *group = new QActionGroup(m_openModeMenu);
    group->setExclusive(true);
    // Only the Windows set has a console mode (see AppConfig::openModes).
    const bool windowsTerminal = openModes.contains(QStringLiteral("console"));
    for (const QString &mode : openModes) {
        auto *action = m_openModeMenu->addAction(openModeLabel(mode, windowsTerminal));
        action->setCheckable(true); action->setChecked(mode == currentMode);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode]() { emit openModeChosen(mode); });
    }
    // Keep terminal options enabled: they are also the way out of Clipboard mode.
    if (terminalTemplateSet)
        m_openModeMenu->setTitle(
            tr("Open in  (own terminal= is set: terminal modes are indistinguishable)"));
}

TrayMenu::~TrayMenu()
{
    // nullptr, если меню уже удалил KStatusNotifierItem -- см. TrayMenu.h.
    delete m_menu.data();
}

void TrayMenu::rebuild(const FleetState &state)
{
    m_menu->clear();
    QAction *dashboard = m_menu->addAction(tr("Open Zerus"));
    connect(dashboard, &QAction::triggered, this, &TrayMenu::sessionsRequested);
    auto *updates=UpdateController::instance();if(updates->updateAvailable()){auto *entry=m_menu->addAction(tr("Update available: Zerus %1").arg(updates->feed().version));entry->setObjectName("updatesAvailable");connect(entry,&QAction::triggered,this,&TrayMenu::updatesRequested);}
    if(updates->restartRequired()){auto *restart=m_menu->addAction(tr("Restart to finish update"));restart->setObjectName("restartToUpdate");connect(restart,&QAction::triggered,updates,&UpdateController::restartIntoInstalledBundle);}
    m_menu->addSeparator();

    // Свой бокс -- первая запись в меню, ведущий разделитель ему не нужен.
    // A control-only computer (hgs.exe on Windows) hosts no sessions: no section.
    const bool localSection = state.local().localSessions;
    if (localSection) {
        const BoxState &box = state.local();
        const bool answered = !box.host.isEmpty();
        addBoxHeader(m_menu, answered ? escapeAmp(box.host) : QStringLiteral("this box"),
                     /*leadingSeparator=*/false);
        if (!answered) {
            addDisabled(m_menu, QStringLiteral("polling…"));
        } else if (std::none_of(box.sessions.cbegin(), box.sessions.cend(), [](const auto &s) { return s.state != "archived"; })) {
            addDisabled(m_menu, QStringLiteral("no sessions"));
        } else {
            for (const SessionInfo &s : box.sessions) {
                if (s.state == "archived") continue;
                const QString text = sessionLabel(s);
                QAction *a = m_menu->addAction(text);
                const QString name = s.name;
                // tty клиента имеет смысл только для СВОЕГО бокса: у пира он указывает
                // на pts на той машине, и совпадение с локальным /dev/pts/<N> было бы
                // случайным -- подняли бы чужое окно (см. ниже, у пиров tty не берём).
                const QString tty = (s.attached > 0 && !s.clients.isEmpty())
                                        ? s.clients.constFirst() : QString();
                connect(a, &QAction::triggered, this, [this, name, tty, saved = s.state != "running"]() {
                    if (saved) emit savedSessionRequested({}, name);
                    else emit sessionActivated(QString(), name, tty);
                });
            }
        }
    }

    // Пиры -- по одной секции на каждого, в порядке peerNames() (QMap -> отсортировано
    // по алиасу, стабильно между перестройками).
    const QList<QString> peers = state.peerNames();
    if (!localSection && peers.isEmpty())
        addDisabled(m_menu, tr("No remote machines"));
    for (const QString &alias : peers) {
        const QString escAlias = escapeAmp(alias);
        addBoxHeader(m_menu, escAlias, /*leadingSeparator=*/localSection || alias != peers.constFirst());

        const qint64 polledAt = state.peerPolledAt(alias);
        if (polledAt == 0) {
            // registerPeer() уже завёл запись, но setPeer() по ней ни разу не отработал.
            addDisabled(m_menu, escAlias + QStringLiteral(": polling…"));
            continue;
        }

        const BoxState *st = state.peer(alias); // не null: alias только что из peerNames()
        if (!st->ok) {
            addDisabled(m_menu, escAlias + QStringLiteral(": unreachable"));
            continue;
        }
        if (std::none_of(st->sessions.cbegin(), st->sessions.cend(), [](const auto &s) { return s.state != "archived"; })) {
            addDisabled(m_menu, QStringLiteral("no sessions"));
            continue;
        }
        for (const SessionInfo &s : st->sessions) {
            if (s.state == "archived") continue;
            const QString text = sessionLabel(s);
            QAction *a = m_menu->addAction(text);
            const QString name = s.name;
            connect(a, &QAction::triggered, this, [this, alias, name, saved = s.state != "running"]() {
                if (saved) emit savedSessionRequested(alias, name);
                else emit sessionActivated(alias, name, QString());
            });
        }
    }

    m_menu->addSeparator();

    // Reuse the submenu and its actions so DBusMenu keeps stable item IDs.
    m_menu->addMenu(m_openModeMenu);

    QAction *refresh = m_menu->addAction(QStringLiteral("Refresh"));
    connect(refresh, &QAction::triggered, this, [this]() { emit refreshRequested(); });

    QAction *quit = m_menu->addAction(QStringLiteral("Quit"));
    connect(quit, &QAction::triggered, this, [this]() { emit quitRequested(); });
}
