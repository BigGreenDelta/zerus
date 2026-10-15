// Window-scope keyboard commands of SessionsWindow: one QAction per command,
// rebound from ShortcutRegistry. See Shortcuts.h and
// docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md.
#include "SessionsWindow.h"
#include "ActivityView.h"
#include "ContentScale.h"
#include "MachinesPage.h"
#include "MessageComposer.h"
#include "SessionList.h"
#include "SessionSearch.h"
#include "SettingsPage.h"
#include "Shortcuts.h"
#include "SwarmButton.h"
#include "TerminalView.h"
#include <QAction>
#include <QApplication>
#include <QFrame>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <algorithm>

namespace {
// A row Next and Previous session stop at: no project header, subagent or launch.
bool sessionRow(const QListWidgetItem *item)
{
    return item && !item->data(SessionRoles::Header).toBool() && item->data(SessionRoles::ChildId).toString().isEmpty()
        && item->data(SessionRoles::LaunchId).toString().isEmpty();
}

// The first widget of area that takes keyboard focus, in focus-chain order.
QWidget *focusTarget(QWidget *area)
{
    if (!area || area->focusProxy()) return area;
    QWidget *widget = area;
    do {
        if ((widget == area || area->isAncestorOf(widget)) && widget->isVisible() && widget->isEnabled()
            && (widget->focusPolicy() & Qt::TabFocus)) return widget;
        widget = widget->nextInFocusChain();
    } while (widget && widget != area);
    return area;
}

// A focused widget's own shortcut, such as Alt+Up in the Projects list, wins over
// a command with the same chord: Qt would find the two ambiguous and run neither.
// While such a widget has focus, the commands' actions leave out those chords,
// so an action's shortcuts() can lack a current chord; read chords from the registry.
const char *const RegistryKeys = "registryKeys";
void setActionKeys(const QHash<QString, QAction *> &actions, const QWidget *window)
{
    QList<QKeySequence> claimed;
    const QWidget *focus = QApplication::focusWidget();
    if (focus && focus->window() == window)
        for (const QWidget *widget = focus; widget && widget != window; widget = widget->parentWidget())
            for (const QAction *action : widget->actions())
                if (action->shortcutContext() != Qt::WidgetShortcut || widget == focus) claimed += action->shortcuts();
    for (auto *action : actions) {
        auto keys = action->property(RegistryKeys).value<QList<QKeySequence>>();
        keys.removeIf([&claimed](const QKeySequence &key) { return claimed.contains(key); });
        if (action->shortcuts() != keys) action->setShortcuts(keys);
    }
}

// Installed on qApp after the router, so it sees each override first and the
// router and Qt's shortcut map then see the keys for the current focus.
class ClaimedKeys : public QObject {
public:
    // Window-scope command actions only: removing a chord from a Composer-scope
    // action because its own editor has focus would disable that command.
    ClaimedKeys(QWidget *window, const QHash<QString, QAction *> &actions) : QObject(window), m_window(window), m_actions(actions) {}
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::ShortcutOverride) return false;
        // Once per override: where it reaches the focus widget, which comes before
        // the ancestors it propagates to; without a focus widget, at the window.
        const QWidget *focus = QApplication::focusWidget();
        if (focus ? watched == focus && focus->window() == m_window : watched == m_window->windowHandle())
            setActionKeys(m_actions, m_window);
        return false;
    }
private:
    QWidget *m_window;
    QHash<QString, QAction *> m_actions;
};

// A native queue holds a message when it shows the message start: its first 24
// non-space characters, with whitespace normalized on both sides, since the
// agent's terminal wraps and indents queued text.
bool queueHolds(const QString &queue, const QString &message)
{
    const QString text = message.simplified();
    qsizetype end = 0;
    for (int count = 0; end < text.size() && count < 24; ++end) if (!text[end].isSpace()) ++count;
    return end > 0 && queue.simplified().contains(text.left(end));
}
}

void SessionsWindow::setupShortcuts()
{
    for (const auto &command : Shortcuts::commands()) {
        if (command.scope != Shortcuts::Scope::Window) continue;
        const QString id = QLatin1String(command.id);
        auto *action = new QAction(Shortcuts::title(command), this);
        action->setObjectName(QStringLiteral("shortcut:") + id);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, [this, id] { runShortcut(id); });
        addAction(action); m_shortcutActions.insert(id, action);
    }
    // Parented to the window: Qt removes a deleted event filter from qApp.
    m_shortcutRouter = new ShortcutRouter(this, Shortcuts::currentPlatform(), this);
    qApp->installEventFilter(m_shortcutRouter);
    qApp->installEventFilter(new ClaimedKeys(this, m_shortcutActions));
    connect(&ShortcutRegistry::instance(), &ShortcutRegistry::changed, this, &SessionsWindow::applyShortcuts);
    applyShortcuts();
    // Inspections stop when another session is selected; the push still ends.
    m_pushTimer.setInterval(1000);
    connect(&m_pushTimer, &QTimer::timeout, this, &SessionsWindow::checkPush);
}

void SessionsWindow::applyShortcuts()
{
    const auto &registry = ShortcutRegistry::instance();
    for (auto it = m_shortcutActions.cbegin(); it != m_shortcutActions.cend(); ++it)
        it.value()->setProperty(RegistryKeys, QVariant::fromValue(registry.keys(it.key())));
    setActionKeys(m_shortcutActions, this);
    // A tooltip is the button's "caption" property or its accessible name, neither
    // of which carries the chord.
    const QList<QPair<QPushButton *, const char *>> buttons{{m_brand, "go.overview"}, {m_sessionsNav, "go.sessions"},
        {m_projectsNav, "go.projects"}, {m_machinesNav, "go.machines"}, {m_accountsNav, "go.accounts"}, {m_settingsNav, "go.settings"},
        {findChild<QPushButton *>("newSession"), "session.new"}, {m_panelNewSession, "session.new"}, {m_inspectorToggle, "panel.toggle"}};
    for (const auto &[button, id] : buttons) {
        if (!button) continue;
        const QString caption = button->property("caption").toString();
        button->setToolTip(registry.tooltip(caption.isEmpty() ? button->accessibleName() : caption, id));
    }
    m_activityView->setQueueSendNowShortcut(registry.hint("queue.sendNow"));
}

QAction *SessionsWindow::shortcutAction(const QString &id) const { return m_shortcutActions.value(id); }

void SessionsWindow::runShortcut(const QString &id)
{
    if (id == "go.overview") m_brand->click();
    // Unlike a second click on the rail button, the command keeps filters.
    else if (id == "go.sessions") showSessionList();
    else if (id == "go.projects") showProjects(selectedHostTarget());
    else if (id == "go.machines") m_machinesNav->click();
    else if (id == "go.accounts") m_accountsNav->click();
    else if (id == "go.settings") showWorkspaceSettings();
    else if (id == "go.anywhere") openGoTo();
    else if (id == "help.shortcuts") openShortcutHelp();
    else if (id == "session.next") stepSession(1, false);
    else if (id == "session.previous") stepSession(-1, false);
    else if (id == "session.nextAttention") stepSession(1, true);
    else if (id == "session.previousAttention") stepSession(-1, true);
    else if (id == "session.new") showNewSession({}, {}, selectedHostTarget());
    else if (id == "session.rename") { if (selected()) renameSession(); }
    else if (id == "session.search") { showSessionList(); m_search->setFocus(); m_search->selectAll(); }
    else if (id == "view.refresh") { emit refreshRequested(); inspect(); m_machinesPage->reload(); }
    else if (id == "tab.next") stepTab(1);
    else if (id == "tab.previous") stepTab(-1);
    else if (id == "tab.terminal") {
        if (!selected() || m_pages->currentIndex() != 0) return;
        if (m_detailTabs->currentWidget() != m_terminal) {
            const int index = m_detailTabs->indexOf(m_terminal);
            if (!m_detailTabs->isTabVisible(index) || !m_detailTabs->isTabEnabled(index)) return;
            m_detailTabs->setCurrentWidget(m_terminal); m_terminal->connectSession(); m_terminal->setFocus(Qt::ShortcutFocusReason);
        } else {
            m_detailTabs->setCurrentIndex(0);
            if (m_composer->isVisible() && m_composer->isEnabled()) m_composer->editor()->setFocus(Qt::ShortcutFocusReason);
        }
    }
    else if (id == "panel.toggle") m_inspectorToggle->toggle();
    else if (id == "focus.next") stepFocus(1);
    else if (id == "focus.previous") stepFocus(-1);
    else if (id == "queue.sendNow") sendQueuedNow();
    else if (id == "zoom.in") stepContentScale(1);
    else if (id == "zoom.out") stepContentScale(-1);
    else if (id == "zoom.reset") stepContentScale(0);
}

bool SessionsWindow::sendQueuedNow(const QString &queueId)
{
    // The running request already shows "Sending…" in Activity.
    if (m_queueSendRequest) return false;
    const auto *entry = selected(); const auto queue = m_details["input_queue"].toObject();
    const QString id = queueId.isEmpty() ? queue["id"].toString() : queueId;
    if (!entry || !entry->online || id.isEmpty() || queue["id"].toString() != id || !queue["can_send_now"].toBool()) {
        showNotice(tr("Nothing queued can be sent now")); return false;
    }
    m_queueSendKey = m_selectedKey;
    m_queueSendRequest = m_client.requestQueueSendNow(entry->host, entry->session.name, m_details["run_id"].toString(), m_details["conversation_id"].toString(), id);
    renderDetails();
    return true;
}

// The message field emits pushRequested right before it sends the message;
// deliverMessage records the request that carries it.
void SessionsWindow::armPush(const QString &key)
{
    if (key != m_selectedKey) return;
    m_push = PendingPush{key, m_details["run_id"].toString(), m_details["conversation_id"].toString(), m_composer->editor()->toPlainText()};
    m_push->deadline.setRemainingTime(m_pushTimeout);
    m_pushTimer.start();
}

void SessionsWindow::pushSendFinished(quint64 request, bool ok, bool uncertain)
{
    if (!m_push || !request || m_push->request != request) return;
    if (!ok || uncertain) endPush(tr("The message was not pushed: sending did not complete"), true);
    else m_push->confirmed = true;
}

// Called with each accepted inspection of the selected session. A push never
// resends text: it only sends the queue snapshot that holds its message.
void SessionsWindow::pushInspected(const QString &key, const QJsonObject &details)
{
    if (!m_push || m_push->key != key) return;
    const QString conversation = details["conversation_id"].toString();
    // A first message may give the run its conversation.
    if (details["run_id"].toString() != m_push->run || (!m_push->conversation.isEmpty() && conversation != m_push->conversation)) {
        endPush(tr("The message was not pushed: the session changed"), true); return;
    }
    m_push->conversation = conversation;
    const auto queue = details["input_queue"].toObject();
    if (m_push->confirmed && queue["can_send_now"].toBool() && queueHolds(queue["text"].toString(), m_push->text)) {
        endPush(); sendQueuedNow(queue["id"].toString()); return;
    }
    checkPush();
}

void SessionsWindow::checkPush()
{
    if (!m_push) { m_pushTimer.stop(); return; }
    if (m_push->deadline.hasExpired()) {
        // The agent may already have taken the message.
        if (m_push->confirmed) endPush(tr("The message was sent without push: it did not appear in the agent's queue"));
        else endPush(tr("The message was not pushed: sending did not complete"), true);
        return;
    }
    // The send ended before reaching the agent: refused, or a dialog was cancelled.
    if (!m_push->request && !QApplication::activeModalWidget() && !m_composer->isSending(m_push->key) && !m_composer->isWaitingToSend(m_push->key))
        endPush(tr("The message was not pushed: sending did not complete"), true);
}

void SessionsWindow::endPush(const QString &notice, bool error)
{
    m_push.reset(); m_pushTimer.stop();
    if (!notice.isEmpty()) showNotice(notice, error);
}

// Next and Previous move in the visible list (search results while shown) and
// stop at its ends. Attention navigation searches the session list in list
// order, wraps, and opens a collapsed project to select its session.
void SessionsWindow::stepSession(int direction, bool attention)
{
    if (!attention) {
        if (m_pages->currentIndex() != 0) showSessionList();
        auto *list = m_listStack->currentIndex() == 1 ? m_searchResults->findChild<QListWidget *>("searchResults") : static_cast<QListWidget *>(m_sessions);
        if (!list) return;
        int row = list->currentRow(); if (row < 0) row = direction > 0 ? -1 : list->count();
        for (row += direction; row >= 0 && row < list->count(); row += direction) {
            auto *item = list->item(row);
            if (item->isHidden() || !sessionRow(item)) continue;
            list->setCurrentRow(row); list->scrollToItem(item); return;
        }
        return;
    }
    if (m_pages->currentIndex() != 0) showSessionList();
    const int count = m_sessions->count();
    int start = m_sessions->currentRow(); if (start < 0) start = direction > 0 ? -1 : count;
    for (int step = 1; step <= count; ++step) {
        const auto *item = m_sessions->item(((start + direction * step) % count + count) % count);
        if (!sessionRow(item)) continue;
        const QString key = item->data(SessionRoles::Key).toString();
        const auto entry = std::find_if(m_entries.cbegin(), m_entries.cend(), [&key](const Entry &e) { return e.key == key; });
        // As the attention badge counts: never an archived session.
        if (entry == m_entries.cend() || entry->session.state == "archived" || !entryNeedsAttention(*entry)) continue;
        revealSession(key); // May rebuild the list.
        for (int row = 0; row < m_sessions->count(); ++row)
            if (sessionRow(m_sessions->item(row)) && m_sessions->item(row)->data(SessionRoles::Key).toString() == key) {
                m_sessions->setCurrentRow(row); m_sessions->scrollToItem(m_sessions->item(row)); break;
            }
        return;
    }
    showNotice(tr("No sessions need attention"));
}

void SessionsWindow::stepTab(int direction)
{
    if (!selected() || m_pages->currentIndex() != 0) return;
    const int count = m_detailTabs->count();
    for (int step = 1; step < count; ++step) {
        const int index = ((m_detailTabs->currentIndex() + direction * step) % count + count) % count;
        if (!m_detailTabs->isTabVisible(index) || !m_detailTabs->isTabEnabled(index)) continue;
        m_detailTabs->setCurrentIndex(index);
        if (auto *target = viewFocusTarget()) target->setFocus(Qt::ShortcutFocusReason);
        return;
    }
}

QWidget *SessionsWindow::viewFocusTarget() const
{
    QWidget *view = m_detailTabs->currentWidget();
    if (view == m_activityStack) return (m_activityStack->currentIndex() == 1 ? m_subagentView : m_activityView)->browser();
    return focusTarget(view);
}

// F6 areas of the Sessions page: the session list, the current view, the
// message field and the side panel. The innermost area holding focus is the
// current one, since the message field lies inside Activity.
void SessionsWindow::stepFocus(int direction)
{
    if (m_pages->currentIndex() != 0) return;
    struct Area { QWidget *area, *target; };
    QList<Area> areas;
    QWidget *list = m_listStack->currentIndex() == 1 ? m_searchResults->findChild<QListWidget *>("searchResults") : m_sessions;
    if (list && list->isVisible()) areas.append(Area{list, list});
    if (selected() && m_detailTabs->isVisible()) areas.append(Area{m_detailTabs->currentWidget(), viewFocusTarget()});
    auto *composer = m_activityStack->currentIndex() == 1 ? m_subagentComposer : m_composer;
    if (composer->isVisible() && composer->isEnabled() && composer->isInputVisible()) areas.append(Area{composer, composer->editor()});
    if (m_inspectorPanel->isVisible()) areas.append(Area{m_inspectorPanel, focusTarget(m_inspector->currentWidget())});
    if (areas.isEmpty()) return;
    const QWidget *focus = QApplication::focusWidget();
    int current = -1;
    for (int i = 0; i < areas.size(); ++i)
        if (focus && (focus == areas[i].area || areas[i].area->isAncestorOf(focus))) current = i;
    const int count = areas.size();
    const int next = current < 0 ? (direction > 0 ? 0 : count - 1) : ((current + direction) % count + count) % count;
    if (areas[next].target) areas[next].target->setFocus(direction > 0 ? Qt::TabFocusReason : Qt::BacktabFocusReason);
}

// Like the Settings slider, in 10% steps within its bounds; 0 resets.
void SessionsWindow::stepContentScale(int direction)
{
    const double scale = direction ? ContentScale::clamp(qRound((ContentScale::factor() + direction * 0.1) * 100) / 100.0) : ContentScale::Default;
    QSettings().setValue("workspace/contentScale", scale);
    applyContentScale(); m_settingsPage->syncContentScale();
    showNotice(tr("Content scale %1%").arg(qRound(scale * 100)));
}
