// Go to (Ctrl+K) and the keyboard cheat sheet (F1) of the workspace window.
// See docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md, "Go to".
#include "SessionsWindow.h"
#include "TasksView.h"
#include "AccountsPage.h"
#include "ComposerDraftStore.h"
#include "GoToDialog.h"
#include "MachinesPage.h"
#include "MessageComposer.h"
#include "ProjectsDialog.h"
#include "SessionList.h"
#include "SessionPresentation.h"
#include "SettingsPage.h"
#include "Shortcuts.h"
#include "ShortcutsDialog.h"
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <algorithm>
#include <limits>

namespace {
constexpr int RecentSessions = 20;
const QList<QPair<QString, const char *>> Sections{
    {"go.overview", QT_TRANSLATE_NOOP("SessionsWindow", "Overview")}, {"go.sessions", QT_TRANSLATE_NOOP("SessionsWindow", "Sessions")},
    {"go.projects", QT_TRANSLATE_NOOP("SessionsWindow", "Projects")}, {"go.machines", QT_TRANSLATE_NOOP("SessionsWindow", "Machines")},
    {"go.accounts", QT_TRANSLATE_NOOP("SessionsWindow", "Accounts")}, {"go.settings", QT_TRANSLATE_NOOP("SessionsWindow", "Settings")}};
}

void SessionsWindow::rememberRecentSession(const QString &key)
{
    if (key.isEmpty()) return;
    m_recentSessions.removeAll(key); m_recentSessions.prepend(key);
    while (m_recentSessions.size() > RecentSessions) m_recentSessions.removeLast();
}

QList<GoToItem> SessionsWindow::goToItems() const
{
    QList<GoToItem> items;
    const auto &registry = ShortcutRegistry::instance();
    // Sessions follow the session list; those it does not show come after them.
    QHash<QString, int> listed;
    for (int row = 0; row < m_sessions->count(); ++row) {
        const auto *item = m_sessions->item(row);
        if (item->data(SessionRoles::Header).toBool() || !item->data(SessionRoles::ChildId).toString().isEmpty()) continue;
        const QString key = item->data(SessionRoles::Key).toString();
        if (!key.isEmpty() && !listed.contains(key)) listed.insert(key, row);
    }
    QList<const Entry *> sessions;
    for (const auto &entry : m_entries) if (entry.session.state != "archived") sessions.append(&entry);
    std::stable_sort(sessions.begin(), sessions.end(), [&listed](const Entry *a, const Entry *b) {
        return listed.value(a->key, std::numeric_limits<int>::max()) < listed.value(b->key, std::numeric_limits<int>::max());
    });
    QHash<QString, int> groupSessions;
    for (const auto *session : sessions) {
        const auto &entry = *session;
        const auto *group = m_organization.group(m_organization.groupFor(entry.identity));
        if (group) ++groupSessions[group->id];
        QStringList detail;
        if (group && !group->name.isEmpty()) detail << group->name;
        detail << entry.machine << status(entry.session, entry.online);
        GoToItem item; item.kind = GoToItem::Session; item.id = entry.key; item.host = entry.host;
        item.title = SessionPresentation::displayTitle(entry.session); item.detail = detail.join(QStringLiteral(" · "));
        item.recency = m_recentSessions.indexOf(entry.key);
        items.append(item);
    }
    for (const auto &group : m_organization.groups()) {
        if (group.name.isEmpty()) continue;
        GoToItem item; item.kind = GoToItem::Project; item.id = group.id; item.title = group.name;
        item.detail = tr("Project · %n session(s)", nullptr, groupSessions.value(group.id));
        items.append(item);
    }
    for (const auto &value : m_accountsPage->profiles()) {
        const auto profile = value.toObject();
        GoToItem item; item.kind = GoToItem::Account; item.id = profile["id"].toString(); item.host = profile["host"].toString();
        item.title = profile["label"].toString().isEmpty() ? item.id : profile["label"].toString();
        item.detail = QStringList{tr("Account"), profile["provider"].toString(), profile["machine"].toString()}.join(QStringLiteral(" · "));
        items.append(item);
    }
    QStringList machines{QString()};
    for (const auto &peer : m_fleet.peerNames()) machines << peer;
    for (const auto &host : machines) {
        const QString name = host.isEmpty() ? m_fleet.local().host : host;
        if (name.isEmpty()) continue;
        GoToItem item; item.kind = GoToItem::Machine; item.id = host; item.host = host; item.title = name;
        item.detail = host.isEmpty() ? tr("Machine · this computer") : tr("Machine");
        items.append(item);
    }
    for (const auto &section : Sections) {
        GoToItem item; item.kind = GoToItem::Section; item.id = section.first; item.title = tr(section.second);
        item.detail = tr("Section"); item.chord = registry.hint(section.first);
        items.append(item);
    }
    // Sections are listed above; Go to itself and message-field commands do
    // not apply from the popup.
    for (const auto &command : Shortcuts::commands()) {
        const QString id = QLatin1String(command.id);
        if (command.scope != Shortcuts::Scope::Window || id.startsWith(QLatin1String("go."))) continue;
        GoToItem item; item.kind = GoToItem::Command; item.id = id; item.title = Shortcuts::title(command);
        item.detail = Shortcuts::group(command); item.chord = registry.hint(id);
        items.append(item);
    }
    return items;
}

void SessionsWindow::openGoTo()
{
    if (!m_goTo) {
        m_goTo = new GoToDialog([this] { return goToItems(); }, this);
        connect(m_goTo, &GoToDialog::activated, this, &SessionsWindow::activateGoTo);
    }
    // Go to's own chords close the popup again.
    m_goTo->setCloseKeys(ShortcutRegistry::instance().keys(QStringLiteral("go.anywhere")));
    m_goTo->popup();
}

const SessionsWindow::Entry *SessionsWindow::draftOwner(const QString &draftKey) const
{
    const auto owner = ComposerDraftStore::owner(draftKey);
    // Only search lists an archived session; its draft offers Restore instead.
    if (owner.kind == ComposerDraftOwner::Kind::Unknown || !owner.archive.isEmpty()) return nullptr;
    const Entry *named = nullptr;
    for (const auto &entry : m_entries) {
        if (entry.session.name != owner.session || entry.session.state == "archived") continue;
        if (entry.host == owner.machine) return &entry;
        // A subagent key names the computer, and this one has no host alias.
        if (!owner.machine.isEmpty() && entry.machine == owner.machine) named = &entry;
    }
    return named;
}

void SessionsWindow::openDraftSession(const QString &draftKey)
{
    const auto *entry = draftOwner(draftKey);
    if (!entry) { showNotice(tr("The session of this draft is not in the session list now."), true); return; }
    const auto owner = ComposerDraftStore::owner(draftKey);
    GoToItem item; item.kind = GoToItem::Session; item.id = entry->key;
    activateGoTo(item);
    const auto *parent = selected();
    if (!parent || parent->key != item.id) return;
    if (!owner.subagent.isEmpty()) {
        // Open the subagent only while it is in the conversation of the draft.
        if (childKey(*parent, owner.subagent) == owner.machine + '\n' + owner.session + '\n' + owner.conversation + '\n' + owner.subagent)
            openSubagent(owner.subagent);
        return;
    }
    if (!m_subagentId.isEmpty()) closeSubagent();
    if (owner.kind == ComposerDraftOwner::Kind::Message && m_composer->isInputVisible()) m_composer->editor()->setFocus(Qt::OtherFocusReason);
    if (owner.kind == ComposerDraftOwner::Kind::Task) {
        m_detailTabs->setCurrentWidget(m_tasks); m_tasks->composer()->editor()->setFocus(Qt::OtherFocusReason);
    }
}

void SessionsWindow::activateGoTo(const GoToItem &item)
{
    switch (item.kind) {
    case GoToItem::Session: {
        // Keep the current filter when the session is listed under it.
        if (m_search->text().isEmpty()) {
            revealSession(item.id);
            for (int row = 0; row < m_sessions->count(); ++row)
                if (m_sessions->item(row)->data(SessionRoles::Key).toString() == item.id
                    && m_sessions->item(row)->data(SessionRoles::ChildId).toString().isEmpty()) {
                    showSessionList(); m_sessions->setCurrentRow(row); m_sessions->scrollToItem(m_sessions->item(row));
                    return;
                }
        }
        for (const auto &entry : m_entries)
            if (entry.key == item.id) { showSession(entry.host, entry.session.name); return; }
        return;
    }
    case GoToItem::Project: {
        const Entry *first = nullptr;
        for (int row = 0; row < m_sessions->count() && !first; ++row) {
            const auto *rowItem = m_sessions->item(row);
            if (rowItem->data(SessionRoles::Header).toBool() || !rowItem->data(SessionRoles::ChildId).toString().isEmpty()) continue;
            for (const auto &entry : m_entries)
                if (entry.key == rowItem->data(SessionRoles::Key).toString() && m_organization.groupFor(entry.identity) == item.id) { first = &entry; break; }
        }
        if (!first)
            for (const auto &entry : m_entries)
                if (entry.session.state != "archived" && m_organization.groupFor(entry.identity) == item.id) { first = &entry; break; }
        if (!first) { showProjects(selectedHostTarget()); m_projectsPage->selectProject(item.id); return; }
        GoToItem session; session.kind = GoToItem::Session; session.id = first->key;
        activateGoTo(session);
        return;
    }
    case GoToItem::Account:
        runShortcut(QStringLiteral("go.accounts")); m_accountsPage->showAccount(item.host, item.id);
        return;
    case GoToItem::Machine:
        runShortcut(QStringLiteral("go.machines")); m_machinesPage->selectMachine(item.host);
        return;
    case GoToItem::Section:
    case GoToItem::Command:
        runShortcut(item.id);
        return;
    }
}

void SessionsWindow::openShortcutHelp()
{
    if (!m_shortcutHelp) {
        m_shortcutHelp = new ShortcutsDialog(ShortcutRegistry::instance(), this);
        connect(m_shortcutHelp, &ShortcutsDialog::customizeRequested, this, [this] {
            m_shortcutHelp->close(); showWorkspaceSettings(); m_settingsPage->openKeyboard();
            activateWindow();
        });
    }
    m_shortcutHelp->show(); m_shortcutHelp->raise(); m_shortcutHelp->activateWindow();
}
