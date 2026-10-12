#include <QtTest>
#include <QAction>
#include <QMenu>
#include <QSignalSpy>
#include "FleetState.h"
#include "TrayMenu.h"

namespace {
const QStringList kModes = {"auto", "tab", "window", "clipboard"};
SessionInfo sess(const QString &name, int attached = 0, const QStringList &clients = {})
{
    SessionInfo s; s.name = name; s.attached = attached; s.clients = clients; return s;
}
BoxState box(const QString &host, const QList<SessionInfo> &sessions, bool ok = true)
{
    BoxState b; b.host = host; b.ok = ok; b.sessions = sessions; return b;
}
QMenu *submenu(QMenu *menu, const QString &title)
{
    for (auto *a : menu->actions()) if (a->menu() && a->menu()->title().startsWith(title)) return a->menu();
    return nullptr;
}
QStringList labels(const QMenu *menu)
{
    QStringList out;
    for (auto *a : menu->actions()) out << (a->isSeparator() ? "---" : a->text());
    return out;
}
FleetState twoBoxFleet()
{
    FleetState state;
    state.setLocal(box("arch", {sess("claude/infra"), sess("codex/games", 1, {"/dev/pts/3"})}), 1000);
    state.registerPeer("mac");
    state.setPeer(box("mac", {sess("kimi/docs", 1, {"/dev/pts/8"})}), 1000);
    return state;
}
}

class TestTrayMenu : public QObject {
    Q_OBJECT
private slots:
    void quickAccessOnly();
    void savedSessionsOpenInManager();
    void archivesStayOutOfQuickAccess();
    void headersAndAmpersandsSurviveExport();
    void submenuIsStableAndDoesNotLeak();
    void openModesRemainSelectable();
    void emitsIntentOnClick();
    void emptyAndBrokenStates();
    void opennessFollowsAboutToShow();
    void survivesExternalMenuDeletion();
    void controlOnlyComputerHasNoSessionSection();
    void windowsModesNameWindowsTerminal();
};

void TestTrayMenu::quickAccessOnly()
{
    TrayMenu tm(kModes, "clipboard", false); tm.rebuild(twoBoxFleet());
    QCOMPARE(labels(tm.menu()), QStringList({"Open Zerus", "---", "arch", "claude/infra", "● codex/games",
        "---", "mac", "● kimi/docs", "---", "Open in", "Refresh", "Quit"}));
    QSignalSpy opened(&tm, &TrayMenu::sessionsRequested);
    tm.menu()->actions().first()->trigger(); QCOMPARE(opened.size(), 1);
}

void TestTrayMenu::savedSessionsOpenInManager()
{
    auto saved = sess("codex/infra"); saved.state = "paused"; saved.resumable = true;
    FleetState state; state.setLocal(box("arch", {saved}), 1000);
    state.setPeer(box("mac", {saved}), 1000);
    TrayMenu tm(kModes, "clipboard", false); tm.rebuild(state);
    QSignalSpy opened(&tm, &TrayMenu::savedSessionRequested), terminal(&tm, &TrayMenu::sessionActivated);
    tm.menu()->actions()[3]->trigger();
    QCOMPARE(opened.at(0), QList<QVariant>({QString(), saved.name}));
    tm.menu()->actions()[6]->trigger();
    QCOMPARE(opened.at(1), QList<QVariant>({QString("mac"), saved.name}));
    QCOMPARE(terminal.size(), 0);
}

void TestTrayMenu::archivesStayOutOfQuickAccess()
{
    auto archived = sess("codex/finished"); archived.state = "archived"; archived.archiveId = "old-one";
    auto state = twoBoxFleet(); auto local = state.local(); local.sessions.append(archived); state.setLocal(local, 1000);
    state.setPeer(box("mac", {archived}), 1000);
    TrayMenu tm(kModes, "clipboard", false); tm.rebuild(state);
    const auto menu = labels(tm.menu());
    QVERIFY(!menu.join("\n").contains("codex/finished"));
    QVERIFY(menu.contains("claude/infra"));
    QCOMPARE(menu.mid(menu.indexOf("mac"), 2), QStringList({"mac", "no sessions"}));
}

void TestTrayMenu::headersAndAmpersandsSurviveExport()
{
    FleetState state; state.setLocal(box("A&B", {sess("claude/R&D")}), 1000);
    TrayMenu tm(kModes, "clipboard", false); tm.rebuild(state);
    auto *header = tm.menu()->actions().at(2);
    QVERIFY(!header->isEnabled()); QVERIFY(!header->isSeparator()); QCOMPARE(header->text(), QString("A&&B"));
    QCOMPARE(tm.menu()->actions().at(3)->text(), QString("claude/R&&D"));
}

void TestTrayMenu::submenuIsStableAndDoesNotLeak()
{
    TrayMenu tm(kModes, "clipboard", false); auto state = twoBoxFleet(); tm.rebuild(state);
    auto *modes = submenu(tm.menu(), "Open in"); QVERIFY(modes);
    const auto actions = modes->actions();
    for (int i = 0; i < 10; ++i) {
        state.setLocal(box("arch", {sess(QString("codex/infra-%1").arg(i))}), 1000);
        tm.rebuild(state);
        QCOMPARE(submenu(tm.menu(), "Open in"), modes);
        QCOMPARE(modes->actions(), actions);
        QCOMPARE(tm.menu()->findChildren<QMenu *>().size(), 1);
        QVERIFY(actions.last()->isChecked());
    }
}

void TestTrayMenu::openModesRemainSelectable()
{
    TrayMenu tm(kModes, "clipboard", true); tm.rebuild(twoBoxFleet());
    auto *modes = submenu(tm.menu(), "Open in"); QVERIFY(modes);
    QVERIFY(modes->title().contains("terminal="));
    QCOMPARE(labels(modes), QStringList({"Automatic", "Tab", "Window", "Clipboard"}));
    QSignalSpy mode(&tm, &TrayMenu::openModeChosen);
    modes->actions().at(1)->trigger();
    QCOMPARE(mode.at(0), QList<QVariant>({QString("tab")}));
    QVERIFY(modes->actions().at(1)->isChecked()); QVERIFY(!modes->actions().last()->isChecked());
    tm.rebuild(twoBoxFleet()); QVERIFY(modes->actions().at(1)->isChecked());
}

void TestTrayMenu::emitsIntentOnClick()
{
    TrayMenu tm(kModes, "clipboard", false); tm.rebuild(twoBoxFleet());
    QSignalSpy activated(&tm, &TrayMenu::sessionActivated);
    QSignalSpy refresh(&tm, &TrayMenu::refreshRequested), quit(&tm, &TrayMenu::quitRequested);
    tm.menu()->actions().at(4)->trigger();
    QCOMPARE(activated.at(0), QList<QVariant>({QString(), QString("codex/games"), QString("/dev/pts/3")}));
    tm.menu()->actions().at(7)->trigger();
    QCOMPARE(activated.at(1), QList<QVariant>({QString("mac"), QString("kimi/docs"), QString()}));
    tm.menu()->actions().at(10)->trigger(); tm.menu()->actions().at(11)->trigger();
    QCOMPARE(refresh.size(), 1); QCOMPARE(quit.size(), 1);
}

void TestTrayMenu::emptyAndBrokenStates()
{
    TrayMenu tm(kModes, "clipboard", false); FleetState state; tm.rebuild(state);
    QCOMPARE(labels(tm.menu()).mid(2, 2), QStringList({"this box", "polling…"}));
    QVERIFY(!tm.menu()->actions().at(3)->isEnabled()); QVERIFY(tm.menu()->actions().first()->isEnabled());
    state.setLocal(box("arch", {}), 1000); state.registerPeer("mac"); tm.rebuild(state);
    QCOMPARE(labels(tm.menu()).mid(2, 5), QStringList({"arch", "no sessions", "---", "mac", "mac: polling…"}));
    state.setPeer(box("mac", {}, false), 2000); tm.rebuild(state);
    QCOMPARE(tm.menu()->actions().at(6)->text(), QString("mac: unreachable"));
    QVERIFY(!tm.menu()->actions().at(6)->isEnabled());
}

void TestTrayMenu::opennessFollowsAboutToShow()
{
    TrayMenu tm(kModes, "clipboard", false); QSignalSpy shown(&tm, &TrayMenu::aboutToShow);
    QVERIFY(!tm.isOpen()); QMetaObject::invokeMethod(tm.menu(), "aboutToShow");
    QVERIFY(tm.isOpen()); QCOMPARE(shown.size(), 1);
    QMetaObject::invokeMethod(tm.menu(), "aboutToHide"); QVERIFY(!tm.isOpen());
}

void TestTrayMenu::survivesExternalMenuDeletion()
{
    auto *tm = new TrayMenu(kModes, "clipboard", false); tm->rebuild(twoBoxFleet());
    delete tm->menu(); QVERIFY(!tm->menu()); delete tm;
}
void TestTrayMenu::controlOnlyComputerHasNoSessionSection()
{
    auto state = twoBoxFleet(); auto local = state.local(); local.sessions.clear(); local.localSessions = false;
    state.setLocal(local, 1000);
    TrayMenu tm(kModes, "auto", false); tm.rebuild(state);
    QCOMPARE(labels(tm.menu()), QStringList({"Open Zerus", "---", "mac", "● kimi/docs", "---", "Open in", "Refresh", "Quit"}));
    FleetState alone; alone.setLocal(local, 1000);
    tm.rebuild(alone);
    QCOMPARE(labels(tm.menu()), QStringList({"Open Zerus", "---", "No remote machines", "---", "Open in", "Refresh", "Quit"}));
    QVERIFY(!tm.menu()->actions()[2]->isEnabled());
}

void TestTrayMenu::windowsModesNameWindowsTerminal()
{
    TrayMenu tm({"auto", "tab", "window", "console", "clipboard"}, "console", false); tm.rebuild(twoBoxFleet());
    QCOMPARE(labels(submenu(tm.menu(), "Open in")),
             QStringList({"Automatic", "Windows Terminal tab", "Windows Terminal window", "Console window", "Clipboard"}));
    TrayMenu other(kModes, "auto", false); other.rebuild(twoBoxFleet());
    QCOMPARE(labels(submenu(other.menu(), "Open in")), QStringList({"Automatic", "Tab", "Window", "Clipboard"}));
}

QTEST_MAIN(TestTrayMenu)
#include "test_traymenu.moc"
