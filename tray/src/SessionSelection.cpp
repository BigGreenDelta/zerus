#include "ArchiveBrowser.h"
#include "NativeSessionCapabilities.h"
#include "SessionsWindow.h"
#include "LocalLaunchWarning.h"
#include "SessionList.h"
#include "MessageComposer.h"
#include "WorkspaceIcons.h"
#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
bool sameInstance(const SessionInfo &a, const SessionInfo &b)
{
    return a.name == b.name && a.archiveId == b.archiveId && a.created == b.created
        && a.runId == b.runId && a.conversationId == b.conversationId && a.state == b.state;
}
// As for a single session, Forget of an OpenCode 2 session removes only its
// Zerus binding, even while it runs. In a selection it is scoped to the row's
// own run and conversation, so a row without both is left as it is.
bool forgetsBinding(const SessionInfo &s)
{
    return s.state != "archived" && NativeSessionCapabilities::forgetsBindingOnly(s);
}
}

QList<SessionsWindow::Entry> SessionsWindow::selectionEntries() const
{
    QSet<QString> keys;
    for (const auto *item : m_sessions->selectedItems())
        if (!item->isHidden() && !item->data(SessionRoles::Header).toBool()
            && item->data(SessionRoles::ChildId).toString().isEmpty() && item->data(SessionRoles::LaunchId).toString().isEmpty())
            keys.insert(item->data(SessionRoles::Key).toString());
    QList<Entry> result;
    for (const auto &entry : m_entries) if (keys.contains(entry.key)) result << entry;
    return result;
}

QString SessionsWindow::selectionActionText(SelectionAction action)
{
    switch (action) {
    case SelectionAction::MarkRead: return tr("Mark as read");
    case SelectionAction::ReviewLater: return tr("Mark needs attention");
    case SelectionAction::CompactRows: return tr("Compact session rows");
    case SelectionAction::ExpandRows: return tr("Expand session rows");
    case SelectionAction::Pause: return tr("Pause");
    case SelectionAction::Resume: return tr("Resume / restore");
    case SelectionAction::Archive: return tr("Move to archive");
    case SelectionAction::Forget: return tr("Forget");
    case SelectionAction::Terminate: return tr("Terminate");
    case SelectionAction::Move: return tr("Move to project");
    }
    return {};
}
QString SessionsWindow::selectionActionIcon(SelectionAction action)
{
    switch (action) {
    case SelectionAction::MarkRead: return "read-all";
    case SelectionAction::ReviewLater: return "attention";
    case SelectionAction::CompactRows: return "compact-row";
    case SelectionAction::ExpandRows: return "expand-row";
    case SelectionAction::Pause: return "pause";
    case SelectionAction::Resume: return "play";
    case SelectionAction::Archive: return "archived";
    case SelectionAction::Forget: return "trash";
    case SelectionAction::Terminate: return "stop";
    case SelectionAction::Move: return "folder-move";
    }
    return {};
}

bool SessionsWindow::selectionActionApplies(const Entry &entry, SelectionAction action) const
{
    const auto &s = entry.session;
    if (action == SelectionAction::CompactRows) return !m_sessionRowPreferences.collapsed(entry.host, s);
    if (action == SelectionAction::ExpandRows) return m_sessionRowPreferences.collapsed(entry.host, s);
    if (action == SelectionAction::Move) return true;
    if (action == SelectionAction::MarkRead) return s.state != "archived" && entryNeedsAttention(entry);
    if (action == SelectionAction::ReviewLater) return s.state != "archived" && !entryNeedsAttention(entry);
    if (!entry.online || isTerminating(entry) || m_composer->isSending(entry.key)
        || std::any_of(m_pendingAnswers.cbegin(), m_pendingAnswers.cend(), [&](const PendingAnswer &p) { return p.key == entry.key; })) return false;
    const auto operation=action==SelectionAction::Pause?"pause":action==SelectionAction::Resume?(s.state=="archived"?"restore":"resume"):
        action==SelectionAction::Archive?"archive":action==SelectionAction::Forget?"forget":"terminate";
    if (!NativeSessionCapabilities::action(s,operation)) return false;
    switch (action) {
    case SelectionAction::Pause: return s.state == "running" && s.resumable && s.activity == "idle" && s.processState != "exited";
    case SelectionAction::Resume: return s.resumable && s.state != "running"
        && (s.state != "archived" || (!s.archiveId.isEmpty() && !archiveNameOccupied(entry)));
    case SelectionAction::Archive: return s.state == "paused" || s.state == "stopped";
    case SelectionAction::Forget: return forgetsBinding(s) ? !s.runId.isEmpty() && !s.conversationId.isEmpty()
        : s.state != "running" && (s.state != "archived" || !s.archiveId.isEmpty());
    case SelectionAction::Terminate: return s.state == "running";
    default: return false;
    }
}

void SessionsWindow::setupSelectionActions()
{
    m_selectionBar = new QFrame(m_sessions); m_selectionBar->setObjectName("sessionSelectionBar");
    m_selectionBar->setFixedHeight(76);
    auto *layout = new QVBoxLayout(m_selectionBar); layout->setContentsMargins(8, 6, 8, 6); layout->setSpacing(4);
    auto *header = new QHBoxLayout; header->setContentsMargins(0, 0, 0, 0); header->setSpacing(8);
    m_selectionCount = new QLabel; m_selectionCount->setObjectName("selectionCount");
    m_selectionCount->setAlignment(Qt::AlignLeft | Qt::AlignVCenter); header->addWidget(m_selectionCount, 1);
    layout->addLayout(header);
    m_selectionTools = new QToolBar; m_selectionTools->setObjectName("selectionTools");
    m_selectionTools->setIconSize(QSize(18, 18)); m_selectionTools->setMovable(false);
    m_selectionTools->setFixedHeight(32); layout->addWidget(m_selectionTools);
    for (const auto action : {SelectionAction::CompactRows, SelectionAction::ExpandRows, SelectionAction::MarkRead, SelectionAction::ReviewLater, SelectionAction::Pause,
            SelectionAction::Resume, SelectionAction::Archive, SelectionAction::Move, SelectionAction::Forget, SelectionAction::Terminate}) {
        auto *control = new QAction(this); control->setObjectName("selection" + selectionActionIcon(action));
        m_selectionActions[action] = control; m_selectionTools->addAction(control);
        connect(control, &QAction::triggered, this, [this, action] {
            const auto entries = selectionEntries();
            if (action != SelectionAction::Move) { runSelectionAction(action, entries); return; }
            auto *menu = new QMenu(this); menu->setObjectName("selectionProjectsMenu"); populateSelectionProjects(menu, entries);
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
            menu->popup(m_selectionBar->mapToGlobal(QPoint(0, 0)));
        });
    }
    auto *clear = new QPushButton; clear->setObjectName("clearSessionSelection");
    header->addWidget(clear);
    connect(clear, &QPushButton::clicked, m_sessions, &SessionList::clearBulkSelection);
    connect(m_sessions, &QListWidget::itemSelectionChanged, this, [this] { if (!m_rebuilding) updateSelectionActions(); });
    connect(m_sessions, &SessionList::bulkSelectionChanged, this, [this] { if (!m_rebuilding) updateSelectionActions(); });
    m_selectionBar->hide();
}

void SessionsWindow::updateSelectionActions()
{
    if (!m_selectionBar) return;
    const auto entries = selectionEntries();
    const bool busy = m_pending || m_settingsRequest || !m_renameKey.isEmpty();
    m_selectionCount->setText(tr("%1 selected").arg(entries.size()));
    const bool themeChanged = !m_selectionBar->property("hgsDark").isValid() || m_selectionBar->property("hgsDark").toBool() != m_dark;
    if (themeChanged) {
        m_selectionBar->setProperty("hgsDark", m_dark);
        m_selectionCount->setStyleSheet(QString("color:%1;font-size:12px;background:transparent;").arg(m_fg));
        m_selectionBar->setStyleSheet(QString("QFrame#sessionSelectionBar {background:%1;border:1px solid %2;border-radius:9px;} QToolBar {border:0;background:transparent;padding:0;spacing:4px;} QToolButton {border:0;background:transparent;padding:7px;} QToolButton:hover {background:%2;border-radius:5px;} QToolButton#qt_toolbar_ext_button {padding:0;min-width:32px;max-width:32px;}")
            .arg(m_dark ? "#25333d" : "#e4edf2", m_dark ? "#526570" : "#acbec8"));
        auto *clear = m_selectionBar->findChild<QPushButton *>("clearSessionSelection");
        configureWorkspaceIconButton(clear, "close", tr("Deselect all (Esc)"), m_dark);
        clear->setStyleSheet(QString("QPushButton {border:0;background:transparent;padding:0;min-width:24px;max-width:24px;min-height:24px;max-height:24px;} QPushButton:hover {background:%1;border-radius:5px;}").arg(m_dark ? "#526570" : "#acbec8"));
        clear->setFixedSize(24, 24); clear->setIconSize(QSize(16, 16));
        if (auto *more = m_selectionTools->findChild<QToolButton *>("qt_toolbar_ext_button")) {
            more->setIcon(workspaceIcon("more", QColor(m_muted))); more->setIconSize(QSize(18, 18));
            more->setToolTip(tr("More actions for selected sessions")); more->setAccessibleName(more->toolTip());
        }
    }
    for (auto it = m_selectionActions.begin(); it != m_selectionActions.end(); ++it) {
        const int count = std::count_if(entries.cbegin(), entries.cend(), [&](const Entry &entry) { return selectionActionApplies(entry, it.key()); });
        it.value()->setText(tr("%1 (%2)").arg(selectionActionText(it.key())).arg(count));
        it.value()->setToolTip(tr("%1 — %2 of %3 selected sessions").arg(selectionActionText(it.key())).arg(count).arg(entries.size()));
        if (themeChanged) it.value()->setIcon(workspaceIcon(selectionActionIcon(it.key()), QColor(m_muted)));
        const bool presentation = it.key() == SelectionAction::CompactRows || it.key() == SelectionAction::ExpandRows;
        it.value()->setVisible(count > 0); it.value()->setEnabled(count > 0 && (presentation || !busy));
        if (auto *button = m_selectionTools->widgetForAction(it.value())) button->setAccessibleName(it.value()->toolTip());
    }
    m_sessions->setSelectionBar(m_selectionBar, !entries.isEmpty() && (m_sessions->bulkSelecting() || entries.size() > 1));
    int archived = 0;
    for (const auto &entry : m_entries) if (entry.session.state == "archived"
        && (m_projectFilters.isEmpty() || m_projectFilters.contains(m_organization.groupFor(entry.identity)))
        && (m_hostFilters.isEmpty() || m_hostFilters.contains(entry.host.isEmpty() ? "@local" : entry.host))
        && selectionActionApplies(entry, SelectionAction::Forget)) ++archived;
    m_clearArchiveAction->setText(m_archiveBrowser->paged()?tr("Remove loaded archived sessions…"):tr("Clear archive…"));
    m_clearArchiveAction->setEnabled(archived > 0 && !busy);
    m_clearArchiveAction->setIcon(workspaceIcon("trash", QColor(m_muted)));
}

void SessionsWindow::populateSelectionProjects(QMenu *menu, const QList<Entry> &entries)
{
    for (const auto &group : m_organization.groups()) {
        auto *action = menu->addAction(QString(group.name).replace('&', "&&")); action->setData(group.id);
        connect(action, &QAction::triggered, this, [this, entries, id = group.id] {
            if (!m_organization.group(id)) return;
            int moved = 0;
            for (const auto &original : entries) {
                const auto current = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &e) { return e.key == original.key && sameInstance(e.session, original.session); });
                if (current == m_entries.cend()) continue;
                m_organization.moveSession(current->identity, id); ++moved;
            }
            saveOrganization(); rebuild(); showNotice(tr("%1 sessions moved to project.").arg(moved));
        });
    }
}

void SessionsWindow::populateSelectionMenu(QMenu *menu, const QList<Entry> &entries)
{
    menu->addSection(tr("%1 selected sessions").arg(entries.size()));
    for (const auto action : {SelectionAction::CompactRows, SelectionAction::ExpandRows, SelectionAction::MarkRead, SelectionAction::ReviewLater, SelectionAction::Pause,
            SelectionAction::Resume, SelectionAction::Archive, SelectionAction::Move, SelectionAction::Forget, SelectionAction::Terminate}) {
        const int count = std::count_if(entries.cbegin(), entries.cend(), [&](const Entry &entry) { return selectionActionApplies(entry, action); });
        if (!count) continue;
        if (action == SelectionAction::Move) {
            auto *projects = menu->addMenu(selectionActionText(action)); projects->setObjectName("moveSelectionToProject");
            populateSelectionProjects(projects, entries); continue;
        }
        auto *item = menu->addAction(workspaceIcon(selectionActionIcon(action), QColor(m_muted)), tr("%1 (%2)").arg(selectionActionText(action)).arg(count));
        item->setObjectName("selection" + selectionActionIcon(action));
        const bool presentation = action == SelectionAction::CompactRows || action == SelectionAction::ExpandRows;
        item->setEnabled(presentation || (!m_pending && !m_settingsRequest && m_renameKey.isEmpty()));
        connect(item, &QAction::triggered, this, [this, action, entries] { runSelectionAction(action, entries); });
    }
    menu->addSeparator(); auto *clear = menu->addAction(tr("Deselect all")); clear->setObjectName("clearSessionSelection");
    connect(clear, &QAction::triggered, m_sessions, &SessionList::clearBulkSelection);
}
void SessionsWindow::showSelectionMenu(const QPoint &position)
{
    qDeleteAll(m_sessionMenu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly)); m_sessionMenu->clear();
    populateSelectionMenu(m_sessionMenu, selectionEntries()); m_sessionMenu->popup(position);
}

void SessionsWindow::clearArchive()
{
    QList<Entry> entries;
    for (const auto &entry : m_entries) if (entry.session.state == "archived"
        && (m_projectFilters.isEmpty() || m_projectFilters.contains(m_organization.groupFor(entry.identity)))
        && (m_hostFilters.isEmpty() || m_hostFilters.contains(entry.host.isEmpty() ? "@local" : entry.host))
        && m_archiveBrowser->includes(entry.session.archivedAt)) entries << entry;
    runSelectionAction(SelectionAction::Forget, entries, true);
}

void SessionsWindow::runSelectionAction(SelectionAction action, const QList<Entry> &entries, bool clearArchive)
{
    if (action == SelectionAction::CompactRows || action == SelectionAction::ExpandRows) {
        const bool collapsed = action == SelectionAction::CompactRows;
        QHash<QString, const Entry *> current;
        for (const auto &entry : m_entries) current.insert(entry.key, &entry);
        int limited = 0;
        for (const auto &original : entries) {
            const auto *entry = current.value(original.key);
            if (!entry || !sameInstance(entry->session, original.session) || !selectionActionApplies(*entry, action)) continue;
            if (!m_sessionRowPreferences.setCollapsed(entry->host, entry->session, collapsed)) ++limited;
        }
        saveSessionRowPreferences(); updateSessionRowPresentation();
        if (limited) showNotice(tr("%1 session rows could not be compacted because the compact row limit was reached. Expand other rows first.").arg(limited), true);
        return;
    }
    if (m_pending || m_settingsRequest || !m_renameKey.isEmpty()) return;
    QList<Entry> targets;
    for (const auto &entry : entries) if (selectionActionApplies(entry, action)) targets << entry;
    if (targets.isEmpty()) return;
    if (action == SelectionAction::Forget || action == SelectionAction::Terminate) {
        QStringList names; for (const auto &entry : targets) names << entry.machine + " / " + entry.session.name;
        QString text = clearArchive ? (m_archiveBrowser->paged()?tr("Remove %1 loaded archived sessions in the current date and machine scope? Unloaded entries are kept."):tr("Remove %1 archived sessions from HGS on the selected machines?")).arg(targets.size())
            : tr("%1 %2 selected sessions?").arg(selectionActionText(action)).arg(targets.size());
        const auto bindings = action == SelectionAction::Forget
            ? std::count_if(targets.cbegin(), targets.cend(), [](const Entry &entry) { return forgetsBinding(entry.session); }) : 0;
        if (action == SelectionAction::Terminate)
            text += tr("\n\nRunning processes will stop. Tracked Codex, Claude and Kimi conversations are kept in Archive; native conversation history is kept.");
        else if (bindings < targets.size())
            text += bindings ? tr("\n\nThese saved or archived entries are removed. Other live sessions and native conversation history are kept.")
                : tr("\n\nOnly these saved or archived entries are removed. Live sessions and native conversation history are kept.");
        if (bindings) text += tr("\n\nOpenCode sessions: only Zerus forgets them; their conversations stay in OpenCode.");
        if (entries.size() != targets.size()) text += tr("\n\n%1 unavailable or inapplicable sessions will be left as they are.").arg(entries.size() - targets.size());
        QMessageBox dialog(QMessageBox::Question, clearArchive ? tr("Clear archive") : selectionActionText(action), text, QMessageBox::Cancel, this);
        dialog.setObjectName("confirmSelectionAction"); dialog.setTextFormat(Qt::PlainText); dialog.setDetailedText(names.join('\n'));
        auto *confirm = dialog.addButton(clearArchive ? tr("Clear archive") : tr("%1 %2 sessions").arg(selectionActionText(action)).arg(targets.size()), QMessageBox::DestructiveRole);
        dialog.setDefaultButton(QMessageBox::Cancel); dialog.setEscapeButton(QMessageBox::Cancel); dialog.exec();
        if (dialog.clickedButton() != confirm) return;
    }
    // Spec D5: one question before resuming or restoring sessions of this computer.
    if (action == SelectionAction::Resume
        && std::any_of(targets.cbegin(), targets.cend(), [](const Entry &entry) { return entry.host.isEmpty(); })
        && (!LocalSessionHost::confirmLaunch(this, m_fleet.local(), {}) || m_pending || m_settingsRequest || !m_renameKey.isEmpty()))
        return;
    if (action == SelectionAction::MarkRead || action == SelectionAction::ReviewLater) {
        int changed = 0;
        for (const auto &entry : targets) {
            changed += action == SelectionAction::MarkRead ? m_fleet.markSessionRead(entry.host, entry.session)
                : m_fleet.setReviewLater(entry.host, entry.session, true);
        }
        ReadMarkStore::add(m_fleet.takeNewReadMarks());
        const auto marks = m_fleet.attentionMarks(), read = m_fleet.readReplies();
        QSettings().setValue("attention/sessionMarks", QJsonDocument(marks).toJson(QJsonDocument::Compact));
        QSettings().setValue("attention/readReplies", QJsonDocument(read).toJson(QJsonDocument::Compact));
        rebuild(); updateDashboard(); emit attentionMarksChanged(marks, read);
        showNotice(tr("%1: %2 of %3 sessions updated.").arg(selectionActionText(action)).arg(changed).arg(targets.size()), changed != targets.size()); return;
    }
    m_selectionOperation = action; m_selectionQueue = targets; m_selectionTotal = targets.size();
    m_selectionDone = 0; m_selectionFailed = 0; m_selectionErrors.clear(); m_selectionRunning = true; m_pending = true;
    m_restoreKey.clear(); updateSelectionActions(); renderDetails(); runNextSelectionAction();
}

void SessionsWindow::runNextSelectionAction()
{
    while (!m_selectionQueue.isEmpty()) {
        const auto original = m_selectionQueue.takeFirst();
        const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &e) { return e.key == original.key; });
        if (found == m_entries.cend() || !sameInstance(found->session, original.session) || !selectionActionApplies(*found, m_selectionOperation)) {
            ++m_selectionFailed; m_selectionErrors << tr("%1 / %2: session changed or is unavailable").arg(original.machine, original.session.name); continue;
        }
        const auto entry = *found; const auto &s = entry.session;
        if (m_selectionOperation == SelectionAction::Archive || m_selectionOperation == SelectionAction::Terminate) {
            m_worktreeCleanup->hide();
            m_archivedWorktree = {entry.host,s.name,s.gitWorktree ? s.gitRoot : QString(),s.gitWorktree ? s.gitCommonDir : QString()};
        }
        showNotice(tr("%1: %2 / %3…").arg(selectionActionText(m_selectionOperation)).arg(m_selectionDone + m_selectionFailed + 1).arg(m_selectionTotal));
        switch (m_selectionOperation) {
        case SelectionAction::Pause:
            m_selectionCommand = "pause " + s.name; m_client.pauseSession(entry.host, s.name); return;
        case SelectionAction::Resume:
            m_selectionCommand = (s.state == "archived" ? "restore " : "resume ") + s.name;
            if (s.state == "archived") {
                const auto key = entry.host + '\n' + s.name;
                if (m_restores.contains(key) || m_restores.size() + m_restoredInspections.size() >= 64) {
                    ++m_selectionFailed; m_selectionErrors << tr("%1: earlier restores still need a refresh").arg(s.name);
                    m_selectionCommand.clear(); continue;
                }
                const auto conversation = entry.key == m_selectedKey
                    ? m_details.value("conversation_id").toString(s.conversationId) : s.conversationId;
                m_restores.insert(key, {entry, conversation, false}); m_selectionRestoreKey = key;
                m_client.restoreArchive(entry.host, s.name, s.archiveId);
            } else m_client.resumeSession(entry.host, s.name);
            return;
        case SelectionAction::Archive:
            m_selectionCommand = "archive " + s.name; m_client.archiveSession(entry.host, s.name); return;
        case SelectionAction::Forget:
            if (forgetsBinding(s)) {
                m_selectionCommand = "forget " + s.name;
                m_selectionForgetRequest = m_client.requestForgetBinding(entry.host, s.name, s.runId, s.conversationId); return;
            }
            m_selectionCommand = (s.state == "archived" ? "forget archive " : "kill ") + s.name;
            if (s.state == "archived") m_client.forgetArchive(entry.host, s.name, s.archiveId); else m_client.killSession(entry.host, s.name); return;
        case SelectionAction::Terminate: {
            const bool keepArchive = s.tracked && QStringList{"codex", "claude", "kimi"}.contains(s.cmd);
            m_selectionCommand = (keepArchive ? "terminate " : "kill ") + s.name;
            if (keepArchive) m_client.terminateSession(entry.host, s.name, s.runId); else m_client.killSession(entry.host, s.name); return;
        }
        default: break;
        }
    }
    m_selectionRunning = false; m_pending = false; m_selectionCommand.clear();
    QString result = tr("%1: %2 of %3 completed.").arg(selectionActionText(m_selectionOperation)).arg(m_selectionDone).arg(m_selectionTotal);
    if (m_selectionFailed) result += tr(" %1 failed or changed; refresh before retrying.").arg(m_selectionFailed) + "\n" + m_selectionErrors.mid(0, 3).join('\n');
    showNotice(result, m_selectionFailed > 0); updateSelectionActions(); renderDetails(); emit refreshRequested();
}

bool SessionsWindow::finishSelectionAction(const QString &operation, bool ok, const QString &detail)
{
    if (!m_selectionRunning) return false;
    // A binding forget finishes only with the result of its own request.
    if (operation != m_selectionCommand || m_selectionForgetRequest) return true;
    if (!m_selectionRestoreKey.isEmpty()) {
        if (auto pending = m_restores.find(m_selectionRestoreKey); pending != m_restores.end()) {
            if (ok) pending->succeeded = true;
            else m_restores.erase(pending);
        }
        m_selectionRestoreKey.clear();
    }
    if (ok) ++m_selectionDone;
    else { ++m_selectionFailed; m_selectionErrors << operation + ": " + detail; }
    m_selectionCommand.clear();
    // Leave the process callback before starting the next machine operation.
    QTimer::singleShot(0, this, &SessionsWindow::runNextSelectionAction); return true;
}
