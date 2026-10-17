#include "ArchiveBrowser.h"
#include "NativeSessionCapabilities.h"
#include "SessionsWindow.h"
#include <QStandardPaths>
#include "WorktreeBatchCleanupDialog.h"
#include "DownloadCache.h"
#include "SessionFileDrop.h"
#include "SessionCardDelegate.h"
#include "SessionPanelDock.h"
#include "ExternalFile.h"
#include "LocalFile.h"
#include "SessionPresentation.h"
#include "AccountUsage.h"
#include "SessionUsage.h"
#include "CacheStatus.h"
#include "ComposerToolbar.h"
#include "AccountUsageStore.h"
#include "IdentityBadge.h"
#include "ProjectAppearance.h"
#include "SessionStatusBadge.h"
#include "SessionElapsed.h"
#include "SessionTag.h"
#include "ProjectsDialog.h"
#include "SwarmController.h"
#include "SwarmDialog.h"
#include "WorktreePanel.h"
#include "NewSessionDialog.h"
#include "ActivityView.h"
#include "ProcessView.h"
#include "AttachmentViewer.h"
#include "NativeBrowserPlugin.h"
#include "NativeUiView.h"
#include "MessageComposer.h"
#include "speech/SpeechComposer.h"
#include "QuestionCard.h"
#include "RecoveryWidgets.h"
#include "SettingsPage.h"
#include "Shortcuts.h"
#include "ContentScale.h"
#include "ActivityWidth.h"
#include "UpdateController.h"
#include "ProcessSettings.h"
#include "TerminalView.h"
#include "TerminalScreen.h"
#include "SessionList.h"
#include "WorkspaceIcons.h"
#include "WorkspaceStyle.h"
#include "BusyIndicator.h"
#include "StatusMessage.h"
#include "WorkspaceFocus.h"
#include "WindowLayer.h"
#include "SwarmButton.h"
#include "MachinesPage.h"
#include "MachineFilter.h"
#include "ProjectFilter.h"
#include "MachineAppearance.h"
#include "MachinePlatform.h"
#include "LocalLaunchWarning.h"
#include "DashboardPage.h"
#include "AccountsPage.h"
#include "SessionSearch.h"
#include "SessionFileReference.h"
#include "SessionGoal.h"
#include <cmath>

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFileDialog>
#include <QStandardPaths>
#include <QProgressBar>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QCheckBox>
#include <QInputDialog>
#include <QWidgetAction>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTabWidget>
#include <QTabBar>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextTable>
#include <QToolTip>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>

namespace {

QJsonObject sendEvidence(const QJsonObject &data)
{
    QJsonObject result;
    for (const auto *field : {"run_id", "conversation_id", "runtime_state", "process_state", "pending_questions", "cache_hint"})
        result.insert(QString::fromLatin1(field), data.value(QString::fromLatin1(field)));
    result.insert("session_usage", QJsonObject{{"prompt_cache", data.value("session_usage").toObject().value("prompt_cache")}});
    return result;
}

bool observedIdentityMatches(const SessionInfo &current, const SessionInfo &original,
                             const QString &run, const QString &conversation)
{
    // Inspection is authoritative and may precede the next fleet refresh.
    // Accept that refresh catching up, while refusing a third incarnation.
    return (current.runId == original.runId && current.conversationId == original.conversationId)
        || (current.runId == run && current.conversationId == conversation);
}

enum Roles { KeyRole = SessionRoles::Key, TitleRole, MetaRole, StatusRole, DetailRole, AgentRole, HostRole, ChildrenRole };

class SessionIdentityLabel : public QLabel {
public:
    using QLabel::QLabel;
    QSize sizeHint() const override {
        // QLabel's wrapped size hint may prefer two lines even when one fits.
        // Prefer the complete ID; heightForWidth still handles a narrower pane.
        const auto insets = contentsMargins();
        return {fontMetrics().horizontalAdvance(text()) + insets.left() + insets.right() + 2 * margin(),
                fontMetrics().height() + insets.top() + insets.bottom() + 2 * margin()};
    }
};

QColor tone(const QString &status, bool dark)
{
    if (status == "Needs approval" || status == "Needs input" || status == "Sign in") return QColor(dark ? "#f0c77b" : "#885400");
    if (status == "Error") return QColor(dark ? "#ff9298" : "#b52b3d");
    if (status == "Background") return QColor(dark ? "#aac0df" : "#466b99");
    if (status == "Working" || status == "Using a tool" || status == "Compacting") return QColor(dark ? "#55e39a" : "#12834e");
    if (status == "Ready") return QColor(dark ? "#78dabb" : "#11775d");
    if (status == "Paused") return QColor(dark ? "#b9a5ed" : "#7051a4");
    return QColor(dark ? "#9aa4b2" : "#646f7e");
}

using SessionPresentation::displayTitle;
using SessionPresentation::sessionLabel;
using SessionPresentation::projectContext;
using SessionPresentation::currentActivity;
using SessionPresentation::currentAction;
using SessionPresentation::scheduled;
using SessionPresentation::childCount;


QString eventTitle(const QString &type)
{
    static const QMap<QString, QString> titles{
        {"SessionStart", "Session started"}, {"SessionEnd", "Session closed"},
        {"UserPromptSubmit", "You sent a message"}, {"TurnStarted", "Turn started"},
        {"UserPromptQueued", "Message queued"}, {"PreToolUse", "Tool started"},
        {"PostToolUse", "Tool finished"}, {"PostToolUseFailure", "Tool failed"},
        {"PermissionRequest", "Approval requested"}, {"PermissionResult", "Approval resolved"},
        {"Stop", "Response finished"}, {"StopFailure", "Response failed"},
        {"Interrupt", "Turn interrupted"}, {"PreCompact", "Compacting context"},
        {"PostCompact", "Context compacted"}, {"SubagentStart", "Subagent started"},
        {"SubagentStop", "Subagent finished"}, {"TaskStarted", "Background task started"},
        {"Notification", "Agent notification"}};
    return titles.value(type, type);
}

QString clockText(double seconds)
{
    return QDateTime::fromSecsSinceEpoch(qint64(seconds)).toLocalTime().toString("HH:mm:ss");
}

QString htmlText(const QString &text)
{
    return text.toHtmlEscaped().replace('\n', "<br>");
}

struct SessionTooltip { QString plain, html; };

SessionTooltip sessionTooltip(const QStringList &fields, int width)
{
    // Tool excerpts can contain minified JSON, base64 or a single very long
    // command. Explicit measured lines also bound unbreakable tokens: a CSS
    // width on its own does not stop Qt rich-text tooltips from expanding.
    const QFont font = QToolTip::font();
    const QFontMetrics metrics(font);
    QStringList lines;
    bool truncated = false;
    constexpr int MaxLines = 11, MaxFieldLines = 2, MaxFieldCharacters = 600;
    for (const auto &raw : fields) {
        const QString field = raw.simplified();
        if (field.isEmpty()) continue;
        if (lines.size() >= MaxLines) { truncated = true; break; }
        const QString text = field.left(MaxFieldCharacters);
        QTextLayout layout(text, font);
        QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option); layout.beginLayout();
        int end = 0, fieldLines = 0;
        while (fieldLines < MaxFieldLines && lines.size() < MaxLines) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            end = line.textStart() + line.textLength();
            lines.append(text.mid(line.textStart(), line.textLength()).trimmed());
            ++fieldLines;
        }
        layout.endLayout();
        if (end < field.size()) {
            truncated = true;
            if (!lines.isEmpty()) lines.last() = metrics.elidedText(lines.last() + QChar(0x2026), Qt::ElideRight, width);
        }
    }
    if (truncated) lines.append(QObject::tr("Open Activity for details."));
    const QString plain = lines.join('\n');
    return {plain, QString("<qt><table width='%1' cellspacing='0' cellpadding='0'><tr><td>%2</td></tr></table></qt>")
                       .arg(width).arg(htmlText(plain))};
}

void updateBrowser(QTextBrowser *browser, const QString &html, QString &previous, bool preservePosition = true)
{
    if (html == previous && preservePosition) return;
    previous = html;
    const int scroll = preservePosition ? browser->verticalScrollBar()->value() : 0;
    const auto cursor = browser->textCursor();
    const auto selection = preservePosition ? cursor.selectedText() : QString();
    const bool updatesEnabled = browser->updatesEnabled();
    browser->setUpdatesEnabled(false);
    browser->setHtml(html);
    if (!selection.isEmpty()) {
        // Counters above the selected field can change length. Restore the
        // nearest surviving occurrence instead of stale character offsets.
        QTextCursor found(browser->document()), closest;
        while (!(found = browser->document()->find(selection, found)).isNull())
            if (closest.isNull() || qAbs(found.selectionStart() - cursor.selectionStart())
                < qAbs(closest.selectionStart() - cursor.selectionStart())) closest = found;
        if (!closest.isNull()) {
            if (cursor.position() < cursor.anchor()) {
                const int start = closest.selectionStart();
                closest.setPosition(closest.selectionEnd());
                closest.setPosition(start, QTextCursor::KeepAnchor);
            }
            browser->setTextCursor(closest);
        }
    }
    browser->verticalScrollBar()->setValue(scroll);
    browser->setUpdatesEnabled(updatesEnabled);
}


QLabel *label(const QString &text, const char *name, QWidget *parent = nullptr)
{
    auto *l = new QLabel(text, parent); l->setObjectName(QLatin1String(name));
    l->setTextFormat(Qt::PlainText); return l;
}
}

QString SessionsWindow::status(const SessionInfo &s, bool reachable)
{
    return SessionPresentation::status(s, reachable);
}

SessionsWindow::SessionsWindow(const QString &hgsPath, QWidget *parent)
    : QWidget(parent), m_client(hgsPath, this)
{
    WorkspaceFocus::install();
    const QString version = QCoreApplication::applicationVersion();
    setObjectName("sessionsWindow"); setWindowTitle(QStringLiteral("Zerus"));
    resize(1200, 790); setMinimumSize(840, 560);
    QSettings settings;
    const auto expanded = settings.value("workspace/expandedSessions").toStringList();
    m_expandedSessions = QSet<QString>(expanded.begin(), expanded.end());
    m_savedOrganization = settings.value("workspace/organization").toByteArray();
    const auto savedProjects = QJsonDocument::fromJson(m_savedOrganization).object();
    if (!savedProjects.isEmpty() && !savedProjects.contains("projects") && !settings.contains("workspace/organizationBeforeProjects"))
        settings.setValue("workspace/organizationBeforeProjects", m_savedOrganization);
    m_organization = SessionOrganization(savedProjects);
    m_pendingLaunches = QJsonDocument::fromJson(settings.value("workspace/pendingLaunches").toByteArray()).object();
    if (!settings.value("sessions/geometry").toByteArray().isEmpty()) restoreGeometry(settings.value("sessions/geometry").toByteArray());
    auto *root = new QHBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(0);
    auto *sidebar = new QWidget; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(56);
    auto *side = new QVBoxLayout(sidebar); side->setContentsMargins(7, 10, 7, 10); side->setSpacing(8);
    auto iconButton = [](const QString &glyph, const QString &caption, const char *name) {
        auto *button = new QPushButton; button->setObjectName(QLatin1String(name));
        button->setProperty("glyph", glyph); button->setToolTip(caption); button->setAccessibleName(caption);
        button->setIconSize(QSize(20, 20)); button->setFixedSize(34, 32); return button;
    };
    m_brand = new SwarmButton; m_brand->setObjectName("brandMark");
    m_brand->setFixedSize(42,44); m_brand->setCheckable(true); m_brand->setProperty("caption", tr("Home — Zerus %1").arg(version)); m_brand->setAccessibleName(tr("Home dashboard"));
    connect(m_brand,&QPushButton::clicked,this,[this] { m_pages->setCurrentIndex(3); m_brand->setChecked(true); emit refreshRequested(); });
    side->addWidget(m_brand); side->addSpacing(8);
    auto *create = iconButton("add", tr("New session"), "newSession");
    create->setFixedSize(42,44);create->setIconSize(QSize(24,24));side->addWidget(create);
    connect(create, &QPushButton::clicked, this, [this]() { showNewSession({}, {}, selectedHostTarget()); });
    m_sessionsNav = iconButton("sessions", tr("Sessions"), "railButton"); m_sessionsNav->setCheckable(true); m_sessionsNav->setChecked(true);
    m_projectsNav = iconButton("projects", tr("Projects"), "railButton"); m_projectsNav->setCheckable(true);
    m_machinesNav = iconButton("machines", tr("Machines and connection status"), "railButton"); m_machinesNav->setCheckable(true);
    m_accountsNav = iconButton("accounts", tr("Agent accounts"), "railButton"); m_accountsNav->setCheckable(true);
    connect(m_accountsNav, &QPushButton::clicked, this, [this] { m_pages->setCurrentIndex(4); m_accountsNav->setChecked(true); });
    for (auto *button : {m_sessionsNav, m_projectsNav, m_machinesNav, m_accountsNav}) { button->setFixedSize(42, 44); button->setIconSize(QSize(24, 24)); side->addWidget(button); }
    m_attentionBadge = new QLabel(m_sessionsNav); m_attentionBadge->setObjectName("railAttentionBadge");
    m_attentionBadge->setAlignment(Qt::AlignCenter); m_attentionBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_attentionBadge->setGeometry(24, 3, 17, 16); m_attentionBadge->hide();
    connect(m_sessionsNav, &QPushButton::clicked, this, [this] {
        if (m_pages->currentIndex() == 0) { m_filter = "all"; m_folderFilterPath.clear(); m_hostFilters.clear(); m_projectFilters.clear(); m_search->clear(); }
        showSessionList();
    });
    connect(m_projectsNav, &QPushButton::clicked, this, [this]() { showProjects(selectedHostTarget()); });
    connect(m_machinesNav, &QPushButton::clicked, this, [this]() {
        m_pages->setCurrentIndex(2); m_machinesNav->setChecked(true); m_sessionsNav->setChecked(false); m_projectsNav->setChecked(false);
    });
    side->addStretch();
    auto *windowPin = m_windowPin = iconButton("unpinned", tr("Keep Zerus above other windows"), "windowPin"); windowPin->setCheckable(true);
    windowPin->setFixedSize(42, 44); windowPin->setIconSize(QSize(24,24)); side->addWidget(windowPin);
    auto *settingsButton = m_settingsNav = iconButton("settings", tr("Settings"), "workspaceSettings"); settingsButton->setCheckable(true); settingsButton->setFixedSize(42, 44); settingsButton->setIconSize(QSize(24,24)); side->addWidget(settingsButton);
    connect(settingsButton, &QPushButton::clicked, this, &SessionsWindow::showWorkspaceSettings);
    auto *versionButton = new QPushButton(UpdateController::buildInfo()["versionName"].toString().section('-', 0, 0));
    versionButton->setObjectName("appVersion"); versionButton->setFixedSize(42, 24);
    versionButton->setCursor(Qt::PointingHandCursor); versionButton->setAutoDefault(false);
    versionButton->setAccessibleName(tr("About Zerus"));
    versionButton->setToolTip(tr("Zerus %1\nOpen About").arg(UpdateController::installedVersion()));
    versionButton->setAccessibleDescription(versionButton->toolTip()); side->addWidget(versionButton);
    connect(versionButton, &QPushButton::clicked, this, [this] { showWorkspaceSettings(); m_settingsPage->openAbout(); });
    // The stylesheet paints each rail button's full rect. macOS layout margins
    // (4 px above, 8 px below a push button) would let neighbours overlap.
    for (int i = 0; i < side->count(); ++i)
        if (auto *widget = side->itemAt(i)->widget()) widget->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    root->addWidget(sidebar);

    auto *body = new QWidget; auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(8, 8, 8, 8); layout->setSpacing(6);
    m_splitter = new QSplitter; m_splitter->setChildrenCollapsible(false); m_splitter->setHandleWidth(8);
    auto *listPanel = new QWidget; listPanel->setObjectName("sessionListPanel"); listPanel->setMinimumWidth(270);
    auto *listLayout = new QVBoxLayout(listPanel); listLayout->setContentsMargins(4, 2, 4, 0); listLayout->setSpacing(7);
    auto *header = new QHBoxLayout; header->setSpacing(4);
    m_heading = label(tr("Sessions"), "heading"); header->addWidget(m_heading, 1);
    auto *groupsMenu = new QMenu(listPanel); groupsMenu->setObjectName("sessionProjectsMenu");
    // Unhandled context events from the margins, filter chips and other panel
    // children bubble here. Text fields and session rows keep their own menus.
    listPanel->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(listPanel, &QWidget::customContextMenuRequested, this, [listPanel, groupsMenu](const QPoint &position) {
        groupsMenu->popup(listPanel->mapToGlobal(position));
    });
    auto *newSession = groupsMenu->addAction(tr("New session…"));
    connect(newSession, &QAction::triggered, this, [this]() { showNewSession({}, {}, selectedHostTarget()); });
    auto *newGroup = groupsMenu->addAction(tr("New project…")); newGroup->setObjectName("createSessionGroup");
    connect(newGroup, &QAction::triggered, this, [this]() { createGroup(); }); groupsMenu->addSeparator();
    for (bool collapse : {false, true}) connect(groupsMenu->addAction(collapse ? tr("Collapse all projects") : tr("Expand all projects")), &QAction::triggered, this, [this, collapse]() {
        const auto groups = m_organization.groups(); for (const auto &g : groups) m_organization.setCollapsed(g.id, collapse);
        saveOrganization(); rebuild();
    });
    m_markAllReadAction = new QAction(tr("Mark all as read"), this); m_markAllReadAction->setObjectName("markAllSessionsRead");
    m_markAllReadAction->setToolTip(tr("Mark current replies across all machines as read, including hidden sessions. Pending approvals, questions and errors stay highlighted."));
    m_markAllReadAction->setEnabled(false);
    connect(m_markAllReadAction, &QAction::triggered, this, &SessionsWindow::markAllRepliesRead);
    groupsMenu->addSeparator(); groupsMenu->addAction(m_markAllReadAction); groupsMenu->setToolTipsVisible(true);
    m_clearArchiveAction = new QAction(tr("Clear archive…"), this); m_clearArchiveAction->setObjectName("clearArchive");
    connect(m_clearArchiveAction, &QAction::triggered, this, &SessionsWindow::clearArchive);
    groupsMenu->addAction(m_clearArchiveAction);
    auto *batchButton = iconButton("more", tr("Session and machine actions"), "batchActions");
    auto *batchMenu = new QMenu(batchButton); batchButton->setMenu(batchMenu); batchMenu->setToolTipsVisible(true);
    connect(batchMenu, &QMenu::aboutToShow, this, [this, batchMenu]() { populateBatchActions(batchMenu); });
    m_panelNewSession = iconButton("add", tr("New session"), "sessionPanelNewSession"); header->addWidget(m_panelNewSession);
    connect(m_panelNewSession, &QPushButton::clicked, this, [this]() { showNewSession({}, {}, selectedHostTarget()); });
    m_sessionsToggle = iconButton("collapse-sessions", tr("Collapse session list"), "sessionPanelToggle"); header->addWidget(m_sessionsToggle);
    header->addWidget(batchButton); m_batchButton = batchButton; m_sessionHeader = header;
    listLayout->addLayout(header);
    m_search = new QLineEdit; m_search->setObjectName("search"); m_search->setPlaceholderText(tr("Search sessions, IDs and messages…"));
    m_search->setClearButtonEnabled(true); m_search->setAccessibleName(tr("Search sessions"));
    connect(m_search, &QLineEdit::textChanged, this, [this]() {
        m_archiveSearchRows.clear();
        m_archiveBrowser->setQuery(m_search->text());
        m_searchResults->setQuery(m_filter=="archived"?QString():m_search->text());
        m_listStack->setCurrentIndex(m_searchResults->active() ? 1 : 0);
        if (!m_searchResults->active()) m_activityView->clearSearchResult();
        rebuild();
    }); listLayout->addWidget(m_search);
    // In the strip, search opens the panel over the conversation with focus in the field.
    m_stripSearch = iconButton("search", tr("Search sessions"), "sessionStripSearch"); m_stripSearch->hide();
    auto *stripSearchRow = new QHBoxLayout; stripSearchRow->setContentsMargins(SessionStrip::tileColumn());
    stripSearchRow->addWidget(m_stripSearch, 0, Qt::AlignHCenter); listLayout->addLayout(stripSearchRow);
    connect(m_stripSearch, &QPushButton::clicked, this, [this] { m_focusSearch = true; m_sessionDock->peek(); });
    connect(m_search, &QLineEdit::textEdited, this, [this]() { m_restoreKey.clear(); m_renameKey.clear(); });
    auto *filterRow = m_filterRow = new QHBoxLayout; filterRow->setSpacing(3);
    const QList<QPair<QString, QString>> filters{{"all", tr("All sessions")}, {"attention", tr("Needs attention")},
        {"active", tr("Active")}, {"paused", tr("Saved sessions")}, {"archived", tr("Archive")}};
    for (const auto &pair : filters) {
        auto *button = new QPushButton; button->setObjectName("sessionFilter"); button->setProperty("glyph", pair.first);
        button->setProperty("filter", pair.first); button->setProperty("caption", pair.second); button->setIconSize(QSize(16, 16));
        button->setCheckable(true); button->setChecked(pair.first == "all"); button->setAccessibleName(pair.second);
        filterRow->addWidget(button, 1); m_filters.append(button);
        connect(button, &QPushButton::clicked, this, [this, id = pair.first]() {
            m_restoreKey.clear(); m_renameKey.clear(); m_pages->setCurrentIndex(0); m_projectsNav->setChecked(false); m_filter = id; m_searchResults->setQuery(id=="archived"?QString():m_search->text());m_listStack->setCurrentIndex(m_searchResults->active()?1:0);if(id=="archived")m_archiveBrowser->setQuery(m_search->text()); if(id=="all")m_folderFilterPath.clear(); rebuild();
        });
    }
    m_machineFilter = new MachineFilter;
    m_projectFilter = new ProjectFilter;
    filterRow->addWidget(m_machineFilter->button()); filterRow->addWidget(m_projectFilter->button());
    listLayout->addLayout(filterRow); listLayout->addWidget(m_machineFilter); listLayout->addWidget(m_projectFilter);
    connect(m_projectFilter, &ProjectFilter::selectionChanged, this, [this](const QSet<QString> &projects) { m_projectFilters = projects; rebuild(); });
    connect(m_machineFilter, &MachineFilter::selectionChanged, this, [this](const QSet<QString> &hosts) { m_hostFilters = hosts; rebuild(); });
    m_archiveBrowser=new ArchiveBrowser(m_client,this);listLayout->addWidget(m_archiveBrowser);
    connect(m_archiveBrowser,&ArchiveBrowser::changed,this,[this]{if(m_filter=="archived")rebuild();});
    connect(m_archiveBrowser,&ArchiveBrowser::windowChanged,this,[this]{if(m_filter=="archived")m_sessions->scrollToTop();});
    m_folderFilterClear=new QPushButton;m_folderFilterClear->setObjectName("clearFolderFilter");m_folderFilterClear->hide();m_folderFilterClear->setAutoDefault(false);
    connect(m_folderFilterClear,&QPushButton::clicked,this,[this]{m_folderFilterPath.clear();rebuild();});listLayout->addWidget(m_folderFilterClear);
    // Holds the height of filter rows the strip hides, so the list does not move.
    m_stripSpacer = new QWidget; m_stripSpacer->hide(); listLayout->addWidget(m_stripSpacer);
    m_sessions = new SessionList; m_sessions->setObjectName("sessionList"); m_sessions->setMinimumWidth(250);
    m_sessions->setProperty("compact", settings.value("workspace/compact", true).toBool());
    m_sessions->setItemDelegate(new SessionDelegate(m_sessions)); m_sessions->setMouseTracking(true);
    m_sessions->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); m_sessions->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sessions->setAccessibleName(tr("Agent sessions; Ctrl-click to select several; drag to organize, left/right to collapse or expand a group"));
    setupSelectionActions();
    m_listStack = new QStackedWidget; m_listStack->addWidget(m_sessions);
    m_searchResults = new SessionSearch(hgsPath); m_listStack->addWidget(m_searchResults); listLayout->addWidget(m_listStack, 1);
    connect(m_searchResults,&SessionSearch::archiveSessionsReady,this,[this](const QString &host,const QJsonArray &rows){m_archiveSearchRows[host]=HgsClient::parseSessions(rows);rebuild();});
    connect(m_searchResults, &SessionSearch::resultActivated, this,
        [this](const QString &host, const QString &name, const QString &archive, const QString &run, const QJsonObject &event, const QString &query) {
            const QString key = host + '\n' + name + (archive.isEmpty() ? QString() : '\n' + archive);
            const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &e) { return e.key == key; });
            if (found == m_entries.cend() || (!run.isEmpty() && found->session.runId != run)) {
                showNotice(tr("This session changed. Search again to see its current history."), true); return;
            }
            pushSelectionChanged(key);
            // The search list owns selection while it is shown, including archive
            // hits. Keep the regular list's selection in sync for normal navigation.
            m_selectedKey.clear();
            for (int i = 0; i < m_sessions->count(); ++i) if (m_sessions->item(i)->data(KeyRole).toString() == key) {
                const QSignalBlocker block(m_sessions); m_sessions->setCurrentRow(i); break;
            }
            closeSubagent();
        m_selectedKey = key; m_details = {}; m_events = {}; m_cursor = 0; m_inspectError.clear(); m_childrenHtml.clear();
            m_activityView->setSessionKey(key); m_composer->setSessionKey(key);
            m_terminal->setSession(m_client.executable(), host, name);
            m_detailTabs->setCurrentIndex(0); inspect(); renderDetails();
            if (!event.isEmpty()) m_activityView->showSearchResult(event, query);
            else m_activityView->clearSearchResult();
        });
    m_count = label({}, "listSummary"); m_count->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *summary = new QHBoxLayout; summary->addWidget(m_count, 1);
    auto *drafts = m_savedDrafts = iconButton("drafts", tr("Saved drafts"), "savedDrafts"); drafts->setAutoDefault(false);
    drafts->setToolTip(tr("Saved drafts: recover local drafts, including those from ended sessions")); summary->addWidget(drafts);
    connect(drafts, &QPushButton::clicked, this, [this] {
        (m_subagentId.isEmpty() ? m_composer : m_subagentComposer)->showSavedDrafts();
    }); listLayout->addLayout(summary);
    // The panel floats over this placeholder so it can collapse into a strip and
    // grow over the conversation without resizing it.
    auto *sessionSlot = new QWidget; sessionSlot->setObjectName("sessionListSlot"); sessionSlot->setMinimumWidth(listPanel->minimumWidth());
    m_splitter->addWidget(sessionSlot); listPanel->setParent(body); listPanel->setAttribute(Qt::WA_StyledBackground);
    m_sessions->installEventFilter(this);
    m_sessions->setContextMenuPolicy(Qt::CustomContextMenu);
    m_sessionMenu = new QMenu(this); m_sessionMenu->setObjectName("sessionContextMenu"); m_sessionMenu->setToolTipsVisible(true);
    connect(m_sessions, &QWidget::customContextMenuRequested, this, [this, groupsMenu](const QPoint &position) {
        const auto *item = m_sessions->itemAt(position);
        const auto global = m_sessions->viewport()->mapToGlobal(position);
        if (selectionEntries().size() > 1) { showSelectionMenu(global); return; }
        if (item && item->data(SessionRoles::Header).toBool()) showGroupMenu(item->data(SessionRoles::Group).toString(), global);
        else if (item && !item->data(SessionRoles::LaunchId).toString().isEmpty()) {
            const auto id = item->data(SessionRoles::LaunchId).toString();
            auto *menu = new QMenu(this); menu->setObjectName("pendingLaunchMenu");
            auto *dismiss = menu->addAction(tr("Hide launch status"));
            connect(dismiss, &QAction::triggered, this, [this, id] {
                if (!m_pendingLaunches.contains(id)) return;
                auto launch = m_pendingLaunches.value(id).toObject(); launch["show_pending"] = false;
                m_pendingLaunches[id] = launch; savePendingLaunches(); rebuild();
            });
            connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater); menu->popup(global);
        }
        else if (item && !item->data(SessionRoles::ChildId).toString().isEmpty()) return;
        else if (item && sessionCardRect(m_sessions->visualItemRect(item)).contains(position)) showSessionMenu(item->data(KeyRole).toString(), global);
        else groupsMenu->popup(global);
    });
    connect(m_sessions, &SessionList::groupToggled, this, [this](const QString &id) {
        if (const auto *group = m_organization.group(id)) m_organization.setCollapsed(id, !group->collapsed);
        saveOrganization(); rebuild();
    });
    connect(m_sessions, &SessionList::childrenToggled, this, [this](const QString &key) {
        if (m_expandedSessions.contains(key)) {
            m_expandedSessions.remove(key);
            if (m_selectedKey == key && !m_subagentId.isEmpty()) closeSubagent();
        } else m_expandedSessions.insert(key);
        QSettings().setValue("workspace/expandedSessions", QStringList(m_expandedSessions.begin(), m_expandedSessions.end()));
        rebuild();
    });
    connect(m_sessions, &SessionList::sessionMoved, this, [this](const QString &id, const QString &group, const QString &before) {
        m_organization.moveSession(id, group, before); saveOrganization(); rebuild();
    });
    connect(m_sessions, &SessionList::groupMoved, this, [this](const QString &id, const QString &before) {
        m_organization.moveGroup(id, before); saveOrganization(); rebuild();
    });
    connect(m_sessions, &SessionList::dragFinished, this, &SessionsWindow::rebuild);
    connect(m_sessions, &QListWidget::currentRowChanged, this, [this]() { if (!m_rebuilding) selectSession(); });
    connect(m_sessions, &QListWidget::itemActivated, this, &SessionsWindow::jumpToLatestActivity);

    m_detailStack = new QStackedWidget; m_detailStack->setObjectName("detail");
    auto *empty = new QWidget; auto *emptyLayout = new QVBoxLayout(empty); emptyLayout->addStretch();
    auto *emptyTitle = label(tr("A little more focus."), "emptyTitle"); emptyTitle->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyTitle);
    auto *emptyText = label(tr("Select a session to see what your agent is doing.\nNew sessions appear here automatically."), "muted");
    emptyText->setAlignment(Qt::AlignCenter); emptyText->setWordWrap(true); emptyLayout->addWidget(emptyText); emptyLayout->addStretch();
    m_detailStack->addWidget(empty);
    auto *details = new QWidget;details->setObjectName("sessionDetailPanel"); auto *detail = new QVBoxLayout(details); detail->setContentsMargins(12, 8, 12, 10); detail->setSpacing(4);
    auto *badges = new QHBoxLayout; badges->setSpacing(4);
    m_title = label({}, "detailTitle"); m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); m_title->setMinimumWidth(70);
    m_sessionLoading = new BusyIndicator(m_title); m_sessionLoading->setObjectName("sessionLoading");
    m_sessionLoading->setAccessibleName(tr("Loading data from the remote machine"));
    m_sessionLoadingTimer.setSingleShot(true);
    connect(&m_sessionLoadingTimer, &QTimer::timeout, this, &SessionsWindow::updateSessionLoading);
    connect(&m_client, &HgsClient::inspectionLoadingChanged, this,
        [this](const QString &host, const QString &name, const QString &archive, const QString &agent, bool loading) {
            if (host.isEmpty()) return;
            const auto key = host + '\n' + name + '\n' + archive + '\n' + agent;
            if (loading) {
                if (!m_sessionReads.contains(key)) {
                    SessionRead read{host, name, archive, agent, {}}; read.age.start(); m_sessionReads.insert(key, read);
                }
            } else m_sessionReads.remove(key);
            updateSessionLoading();
        });
    badges->addWidget(m_title, 1);
    connect(&m_client,&HgsClient::sessionReferenceReady,this,[this](const QString &uri){emit copyTextRequested(uri);showNotice(tr("Session reference copied"));});
    connect(&m_client,&HgsClient::sessionReferenceFailed,this,[this](const QString &error){showNotice(error,true);});
    m_badge = label({}, "badge"); m_badge->setFixedHeight(24);
    badges->addWidget(m_badge, 0, Qt::AlignVCenter); badges->addSpacing(6);
    m_model = label({}, "sessionModel", details); m_model->setMaximumWidth(260);
    m_open = iconButton("external", tr("Open externally"), "openSessionAction");
    m_shell = iconButton("terminal", tr("Open shell in session folder"), "openFolderShellAction");
    connect(m_shell, &QPushButton::clicked, this, &SessionsWindow::openFolderShell);
    m_fileManager = iconButton("folder", tr("Open session folder in file manager"), "openSessionFolderAction");
    connect(m_fileManager, &QPushButton::clicked, this, &SessionsWindow::openSessionFolder);
    m_pause = iconButton("pause", tr("Pause session"), "pauseAction");
    connect(m_open, &QPushButton::clicked, this, &SessionsWindow::openSession);
    connect(m_pause, &QPushButton::clicked, this, &SessionsWindow::changeSession);
    badges->addWidget(m_fileManager); badges->addWidget(m_shell); badges->addWidget(m_open); badges->addWidget(m_pause);
    m_more = iconButton("more", tr("Session actions"), "more");
    for (auto *button : {m_fileManager, m_shell, m_open, m_pause, m_more}) {
        button->setProperty("sessionHeaderAction", true);
        configureWorkspaceIconButton(button, button->property("glyph").toString(), button->toolTip(), m_dark);
    }
    auto *moreMenu = new QMenu(m_more);
    m_renameAction = moreMenu->addAction(tr("Rename…")); m_renameAction->setObjectName("renameSessionAction");
    connect(m_renameAction, &QAction::triggered, this, &SessionsWindow::renameSession);
    m_forkAction = moreMenu->addAction(tr("Fork session…")); m_forkAction->setObjectName("forkSessionAction");
    connect(m_forkAction, &QAction::triggered, this, &SessionsWindow::forkSession);
    moreMenu->addSeparator();
    m_clearAction = moreMenu->addAction(tr("Clear session…")); m_clearAction->setObjectName("clearSessionAction");
    m_clearAction->setToolTip(tr("Start this agent's conversation from scratch, like /clear in Terminal. Your unsent draft is kept."));
    connect(m_clearAction, &QAction::triggered, this, &SessionsWindow::clearContext);
    m_freshAction = moreMenu->addAction(tr("Start fresh…")); m_freshAction->setObjectName("freshSessionAction");
    m_freshAction->setToolTip(tr("Start this stopped session with a new conversation and the same folder, account and model."));
    connect(m_freshAction, &QAction::triggered, this, &SessionsWindow::startFresh);
    m_archiveAction = moreMenu->addAction(tr("Move to archive")); m_archiveAction->setObjectName("archiveSessionAction");
    connect(m_archiveAction, &QAction::triggered, this, &SessionsWindow::archiveSession);
    m_forgetAction = moreMenu->addAction(tr("Terminate / forget session…")); m_forgetAction->setObjectName("forgetSessionAction");
    connect(m_forgetAction, &QAction::triggered, this, &SessionsWindow::terminateSession); m_more->setMenu(moreMenu); badges->addWidget(m_more); detail->addLayout(badges);
    m_meta = label({}, "sessionMeta"); m_meta->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_providerBadge = new IdentityBadge(IdentityBadges::Provider); m_providerBadge->setObjectName("sessionProviderBadge");
    m_machineBadge = new IdentityBadge(IdentityBadges::Machine); m_machineBadge->setObjectName("sessionMachineBadge");
    auto *metadata = new QHBoxLayout; metadata->setSpacing(6); metadata->addWidget(m_providerBadge); metadata->addWidget(m_machineBadge); metadata->addWidget(m_meta, 1); metadata->addWidget(m_model);
    detail->addLayout(metadata);
    m_usageStore=new AccountUsageStore(hgsPath,this);
    m_usageStrip=new QWidget;m_usageStrip->setObjectName("sessionUsageStrip");m_usageStrip->setFixedHeight(22);m_usageStrip->hide();
    auto *usageLayout=new QHBoxLayout(m_usageStrip);usageLayout->setContentsMargins(0,0,0,0);usageLayout->setSpacing(4);
    m_accountUsage=new AccountUsage::Button;usageLayout->addWidget(m_accountUsage);
    m_usageRefresh=new AccountUsage::RefreshButton;usageLayout->addWidget(m_usageRefresh);
    connect(m_usageRefresh,&QPushButton::clicked,this,[this]{refreshAccountUsage(true);});
    connect(m_accountUsage,&QPushButton::clicked,this,[this]{
        const auto *entry=selected();if(!entry)return;
        m_pages->setCurrentIndex(4);m_accountsPage->showAccount(entry->host,m_accountUsageData.value("id").toString());
    });
    m_meta->installEventFilter(this); m_title->installEventFilter(this);
    // Context feedback belongs to the fixed composer/footer rows, never the header.
    m_hint = label({}, "hint", details); m_hint->hide();
    m_goal = new QPushButton; m_goal->setObjectName("sessionGoal"); m_goal->hide();
    m_goal->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); detail->addWidget(m_goal);
    connect(m_goal, &QPushButton::clicked, this, [this] { setInspectorVisible(true); m_inspector->setCurrentIndex(0); });
    auto *tabs = new QTabWidget; m_detailTabs = tabs; tabs->setObjectName("sessionDetailTabs");
    tabs->setDocumentMode(true); tabs->tabBar()->setDrawBase(false);
    auto browser = [this]() {
        auto *view = new QTextBrowser; view->setOpenLinks(false); view->setOpenExternalLinks(false);
        view->setFrameShape(QFrame::NoFrame); view->document()->setDocumentMargin(4);
        view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); return view;
    };
    auto *activityPage = new QWidget; auto *activityLayout = new QVBoxLayout(activityPage);
    activityLayout->setContentsMargins(0, 0, 0, 0); activityLayout->setSpacing(0);
    m_activityView = new ActivityView; m_activityView->setObjectName("mainActivity"); m_composer = new MessageComposer; m_question = new QuestionCard;
    auto *voice = new SpeechComposer(hgsPath, m_composer, [this] {
        SpeechComposer::Target target;
        const auto *entry = selected();
        if (!entry || m_details.isEmpty()) return target;
        target.key=entry->key;target.host=entry->host;target.session=entry->session.name;
        target.run=m_details.value("run_id").toString();target.conversation=m_details.value("conversation_id").toString();
        // Voice messages to local Windows sessions are a Phase 2 non-goal (spec D16).
        const bool localWindows=entry->host.isEmpty()&&MachinePlatform::of(m_fleet.local().metrics,true)==MachinePlatform::Style::Windows;
        target.available=!localWindows && entry->online && entry->session.archiveId.isEmpty() && messageBlockReason(*entry).isEmpty();
        if(localWindows)target.reason=tr("Voice messages cannot be sent to sessions on this Windows computer yet.");
        if(CacheStatus::expired(m_details))target.warning=CacheStatus::warning(m_details)+tr("\n\nSend this voice message with the full conversation context?");
        return target;
    });
    connect(voice,&SpeechComposer::messageChanged,this,[this](const QString &key,const QJsonObject &message){
        auto &rows=m_localMessages[key];const auto id=message.value("id").toString();
        for(qsizetype i=rows.size();i-->0;)if(rows[i].toObject().value("message_id")==id)rows.removeAt(i);
        rows.append(QJsonObject{{"id","voice-"+id},{"message_id",id},{"request_id",id},{"source","hgs_voice"},{"type","UserMessage"},
            {"voice",message},{"text",message.value("transcript")},{"status","sent"},{"run_id",message.value("run_id")},
            {"conversation_id",message.value("conversation_id")},{"submitted_at",message.value("created_at")},{"attachments",QJsonArray{message.value("audio")}}});
        if(key==m_selectedKey){reconcileMessages();renderDetails();m_activityView->jumpToLatest();inspect();}
    });
    connect(&m_client, &HgsClient::downloadCacheWarning, this, [this](const QString &error) { showNotice(error, true); });
    auto openFileReference = [this](const QString &href) {
        const auto *entry = selected(); if (!entry) return;
        const auto target = SessionFileReference::parse(href); if (!target.valid()) return;
        const QString host = entry->host, machine = entry->machine;
        const bool local = host.isEmpty(), online = entry->online;
        const QString path = SessionFileReference::resolve(target, m_subagentId.isEmpty() ? selectedDirectory() : m_subagentCwd, local ? QDir::homePath() : QString());
        // Files open after the dialog closes; folders open as they are.
        QUrl desktopTarget; bool desktopFile = false;
        QDialog dialog(this); dialog.setObjectName("sessionFileDialog");
        dialog.setWindowTitle(tr("File reference — Zerus")); dialog.resize(local ? 560 : 650, 230);
        auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(20, 20, 20, 20); layout->setSpacing(12);
        auto *source = new QLabel(local ? tr("File on this machine") : tr("File on %1").arg(machine));
        source->setTextFormat(Qt::PlainText); layout->addWidget(source);
        // This computer shows and copies its paths with native separators; a peer's
        // stay POSIX. path keeps the Qt form that QFileInfo and QUrl read.
        const auto shown = MachinePlatform::native(path.isEmpty() ? target.path : path, MachinePlatform::of(m_fleet.local().metrics, local));
        auto *address = new QLineEdit(shown); address->setReadOnly(true);
        address->setObjectName("sessionFilePath");
        auto *copy = new QPushButton;copy->setObjectName("copySessionFilePath");
        configureWorkspaceIconButton(copy,"copy",tr("Copy path"),m_dark);
        copy->setAutoDefault(false);
        auto *pathRow=new QHBoxLayout;pathRow->setSpacing(8);pathRow->addWidget(address,1);pathRow->addWidget(copy);layout->addLayout(pathRow);
        connect(copy, &QPushButton::clicked, &dialog, [this, address] { emit copyTextRequested(address->text()); });
        if (!target.location.isEmpty()) {
            auto *position = new QLabel(tr("Source position: %1").arg(target.location));
            position->setTextFormat(Qt::PlainText); layout->addWidget(position);
        }
        auto *hint = new QLabel(local ? (path.isEmpty() ? tr("The session folder is unknown. Copy the path to locate this file.")
            : tr("Open with your default application, or copy the path."))
            : tr("Download… lets you choose where to save. Download and open saves a copy in Downloads/Zerus Files and opens it with your default application."));
        hint->setObjectName("sessionFileHint");
        hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        hint->setTextFormat(Qt::PlainText); hint->setWordWrap(true); layout->addWidget(hint);
        auto *buttons = new QDialogButtonBox; layout->addWidget(buttons);
        QString savedPath;
        quint64 pending = 0; bool openAfterDownload = false;
        if (local && !path.isEmpty()) {
            auto *open = buttons->addButton(tr("Open file"), QDialogButtonBox::ActionRole); open->setObjectName("openSessionFile");
            connect(open, &QPushButton::clicked, &dialog, [path, hint, &dialog, &desktopTarget, &desktopFile] {
                if (!QFileInfo::exists(path)) { hint->setText(tr("This file is no longer available at this path.")); return; }
                desktopTarget = QUrl::fromLocalFile(path); desktopFile = true;
                dialog.accept();
            });
            auto *folder = buttons->addButton(tr("Open folder"), QDialogButtonBox::ActionRole); folder->setObjectName("openSessionFileFolder");
            connect(folder, &QPushButton::clicked, &dialog, [path, hint, &dialog, &desktopTarget] {
                const auto directory = QFileInfo(path).absolutePath();
                if (!QFileInfo(directory).isDir()) { hint->setText(tr("This folder is no longer available at this path.")); return; }
                desktopTarget = QUrl::fromLocalFile(directory);
                dialog.accept();
            });
        } else if (!local) {
            auto *progress = new QProgressBar; progress->setObjectName("sessionFileProgress");
            progress->setAccessibleName(tr("Downloading file from %1").arg(machine));
            progress->setRange(0, 0); progress->setTextVisible(false); progress->hide(); layout->insertWidget(layout->count() - 1, progress);
            auto *download = buttons->addButton(tr("Download…"), QDialogButtonBox::ActionRole); download->setObjectName("downloadSessionFile");
            auto *open = buttons->addButton(tr("Download and open"), QDialogButtonBox::ActionRole); open->setObjectName("downloadOpenSessionFile");
            open->setToolTip(tr("Save automatically to %1 and open the local copy.").arg(QDir::toNativeSeparators(DownloadCache::directory())));
            auto *savedOpen = buttons->addButton(tr("Open local copy"), QDialogButtonBox::ActionRole); savedOpen->setObjectName("openDownloadedSessionFile"); savedOpen->hide();
            const QString remotePath = path.isEmpty() && target.path.startsWith("~/") ? target.path : path;
            const bool downloadable = online && !remotePath.isEmpty();
            download->setEnabled(downloadable); open->setEnabled(downloadable);
            if (!online) hint->setText(tr("%1 is offline. Connect to this machine to download its files.").arg(machine));
            else if (remotePath.isEmpty()) hint->setText(tr("The session folder is unknown. Copy the path or use the SSH terminal to locate the file."));
            auto startDownload = [&, download, open, savedOpen, progress, remotePath](bool opening) {
                if (pending) return;
                QString destination = DownloadCache::directory();
                if (!opening) {
                    const auto downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
                    QFileDialog save(&dialog, tr("Save local copy"), QDir(downloads.isEmpty() ? QDir::homePath() : downloads).filePath(QFileInfo(remotePath).fileName()));
                    save.setObjectName("sessionFileSaveDialog"); save.setAcceptMode(QFileDialog::AcceptSave); save.setFileMode(QFileDialog::AnyFile);
                    if (save.exec() != QDialog::Accepted || save.selectedFiles().isEmpty()) return;
                    destination = save.selectedFiles().first();
                }
                download->setEnabled(false); open->setEnabled(false); savedOpen->hide();
                progress->show(); hint->setText(tr("Downloading from %1 to %2…\nThe download continues if you close this dialog.").arg(machine, destination));
                openAfterDownload = opening;
                pending = opening ? m_client.downloadFileToCache(host, remotePath) : m_client.downloadFile(host, remotePath, destination);
                auto *completion = new QObject(this);
                const QPointer<QDialog> shownDialog(&dialog);
                const auto request = pending;
                connect(&m_client, &HgsClient::fileDownloaded, completion, [this, completion, shownDialog, request, opening](quint64 id, const QString &localPath) {
                    if (id != request) return;
                    // Like a browser download, the copy is marked as coming from another machine.
                    ExternalFile::markDownloaded(localPath);
                    if (!shownDialog) {
                        if (opening) QTimer::singleShot(0, this, [this, localPath] { openExternalFile(localPath, true); });
                        else showNotice(tr("Downloaded to %1").arg(localPath));
                    }
                    completion->deleteLater();
                });
                connect(&m_client, &HgsClient::fileDownloadFailed, completion, [this, completion, shownDialog, request](quint64 id, const QString &error) {
                    if (id != request) return;
                    if (!shownDialog) showNotice(tr("Download failed: %1").arg(error), true);
                    completion->deleteLater();
                });
            };
            connect(download, &QPushButton::clicked, &dialog, [startDownload] { startDownload(false); });
            connect(open, &QPushButton::clicked, &dialog, [startDownload] { startDownload(true); });
            connect(&m_client, &HgsClient::fileDownloadFailed, &dialog, [&, progress, download, open, downloadable](quint64 request, const QString &error) {
                if (!pending || request != pending) return;
                pending = 0; progress->hide(); download->setEnabled(downloadable); open->setEnabled(downloadable);
                hint->setText(tr("Download failed: %1").arg(error));
            });
            connect(&m_client, &HgsClient::fileDownloaded, &dialog, [&, progress, download, open, savedOpen, downloadable](quint64 request, const QString &destination) {
                if (!pending || request != pending) return;
                pending = 0; progress->hide(); savedPath = destination;
                download->setEnabled(downloadable); open->setEnabled(downloadable);
                hint->setText(tr("Saved on this computer: %1").arg(destination)); open->hide(); savedOpen->show();
                if (openAfterDownload) { desktopTarget = QUrl::fromLocalFile(destination); desktopFile = true; dialog.accept(); }
            });
            connect(savedOpen, &QPushButton::clicked, &dialog, [&] {
                if (!LocalFile::readable(QFileInfo(savedPath))) { hint->setText(tr("The local copy is no longer available.")); return; }
                desktopTarget = QUrl::fromLocalFile(savedPath); desktopFile = true; dialog.accept();
            });
            auto *ssh = buttons->addButton(tr("Open SSH terminal"), QDialogButtonBox::ActionRole); ssh->setObjectName("openSessionFileSsh");
            connect(ssh, &QPushButton::clicked, &dialog, [this, host, &dialog] { emit machineSshRequested(host); dialog.accept(); });
            auto *close = buttons->addButton(QDialogButtonBox::Close);
            close->setAutoDefault(false);
        }
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
        if (!desktopTarget.isEmpty()) {
            // Wayland defers launch until the focused window receives an
            // activation token. Destroying that window cancels the callback.
            // Dispatch after the modal has gone, using the persistent workspace.
            // Folders take the same path: a macOS bundle folder would launch.
            QTimer::singleShot(0, this, [this, desktopTarget, desktopFile, local] {
                openExternalFile(desktopTarget.toLocalFile(), desktopFile && !local);
            });
        }
    };
    connect(m_activityView, &ActivityView::fileReferenceActivated, this, openFileReference);
    m_nativeSignIn = new QWidget; m_nativeSignIn->setObjectName("nativeSignInBanner");
    auto *signInLayout = new QVBoxLayout(m_nativeSignIn); signInLayout->setContentsMargins(14, 12, 14, 12);
    auto *signInText = new QLabel(tr("Sign in to DeepSeek to start using this session. Complete sign-in in the official DeepSeek UI, then return here."));
    signInText->setWordWrap(true); signInText->setTextFormat(Qt::PlainText); signInLayout->addWidget(signInText);
    auto *signInButton = new QPushButton(tr("Sign in to DeepSeek…")); signInButton->setObjectName("nativeSignInButton");
    signInLayout->addWidget(signInButton, 0, Qt::AlignLeft); m_nativeSignIn->hide();
    connect(signInButton, &QPushButton::clicked, this, [this] {
        const auto *entry = selected();
        if (entry && entry->online && entry->session.cmd == "dsh" && m_details.value("auth_required").toBool())
            showNativeUi(*entry);
    });
    // Activity's transcript keeps the full pane for its scroll bar. The content
    // around it shares the transcript's centered reading column.
    const auto activityColumn = [this](QBoxLayout *page) {
        auto *column = new ActivityWidth::ColumnLayout; page->addLayout(column); m_activityColumns.append(column); return column;
    };
    activityColumn(activityLayout)->addWidget(m_nativeSignIn);
    m_contextUsage=new SessionUsage::ContextButton;m_contextUsage->setObjectName("activityContext");
    activityLayout->addWidget(m_activityView, 1);
    auto *inputColumn = activityColumn(activityLayout); inputColumn->addWidget(m_question);
    m_usageLimit=new ToolbarChip;m_usageLimit->setObjectName("usageLimitChip");m_usageLimit->setTone(ChipTone::Danger);m_usageLimit->setIconName("attention");
    auto *usagePanel=new QWidget;usagePanel->setObjectName("usageLimitPopover");
    auto *limitLayout=new QVBoxLayout(usagePanel);limitLayout->setContentsMargins(14,12,14,12);limitLayout->setSpacing(8);
    m_usageWarning=new QLabel;m_usageWarning->setObjectName("activityUsageWarning");m_usageWarning->setTextFormat(Qt::PlainText);m_usageWarning->setWordWrap(true);
    auto *usageActions=new QHBoxLayout;usageActions->addStretch();
    auto *usageRefresh=new QPushButton(tr("Refresh usage"));usageRefresh->setObjectName("usageLimitRefresh");usageRefresh->setAutoDefault(false);
    usageActions->addWidget(usageRefresh);limitLayout->addWidget(m_usageWarning);limitLayout->addLayout(usageActions);
    m_usageLimit->setPopoverContent(usagePanel);m_composer->toolbar()->add(ComposerToolbar::Slot::UsageLimit,m_usageLimit);
    connect(usageRefresh,&QPushButton::clicked,this,[this]{refreshAccountUsage(true);});
    m_recovery = new RecoveryUi::Panel;
    m_recoveryChip=new ToolbarChip;m_recoveryChip->setObjectName("recoveryChip");m_recoveryChip->setIconName("refresh");
    m_recoveryChip->setPopoverContent(m_recovery);m_composer->toolbar()->add(ComposerToolbar::Slot::Recovery,m_recoveryChip);
    m_recovery->summaryChanged = [this] {
        const auto summary=m_recovery->summary();
        m_recoveryChip->setLabels(summary.label,summary.shortLabel);m_recoveryChip->setTone(summary.tone);
        m_recoveryChip->setDetail(m_recovery->detailText());m_recoveryChip->setActive(m_recovery->active());
    };
    m_recovery->openTerminal = [this] {
        m_recoveryChip->closePopover();
        const auto *entry=selected();if(!entry || !entry->online)return;
        if(entry->session.cmd=="dsh"){showNativeUi(*entry);return;}
        if(!NativeSessionCapabilities::terminal(entry->session,m_details)){showNotice(NativeSessionCapabilities::terminalReason(entry->session,m_details),true);return;}
        m_detailTabs->setCurrentWidget(m_terminal);m_terminal->connectSession();
    };
    m_recovery->refreshUsage = [this] { refreshAccountUsage();inspect();emit refreshRequested(); };
    m_recovery->openSettings = [this] {
        m_recoveryChip->closePopover();showWorkspaceSettings();m_settingsPage->openRecovery();
    };
    m_recovery->action = [this](const QJsonObject &payload) {
        const auto *entry=selected();if(!entry||!entry->online)return;
        m_recoveryActionKey=m_selectedKey;
        m_recoveryAction=m_client.requestRecovery(entry->host,"action",payload);
    };
    connect(&m_client,&HgsClient::recoveryFinished,this,[this](quint64 id,bool ok,const QJsonObject &,const QString &error) {
        if(id!=m_recoveryAction)return;m_recoveryAction=0;
        if(m_recoveryActionKey==m_selectedKey)m_recovery->finished(ok?QString():error);
        inspect();emit refreshRequested();
    });
    auto *toolbar=m_composer->toolbar();
    m_compactCancel = new QPushButton(tr("Cancel send")); m_compactCancel->setObjectName("cancelCompactSend");
    m_compactCancel->setToolTip(tr("Cancel the pending send and unlock the message for editing. Compaction or startup continues; use Stop to interrupt the agent."));
    m_compactCancel->setAutoDefault(false); m_compactCancel->setFixedHeight(20);
    m_compactCancel->setStyleSheet(QStringLiteral("QPushButton { min-height: 20px; max-height: 20px; padding: 0 6px; margin: 0; border: none; background: transparent; } QPushButton:hover { text-decoration: underline; }"));
    m_compactCancel->hide();
    connect(m_compactCancel,&QPushButton::clicked,this,[this] {
        cancelCompactContinuation(m_selectedKey, tr("Pending send cancelled. Your draft is unchanged."));
        if (m_startSend.key == m_selectedKey) cancelStartContinuation(tr("Pending send cancelled. Your draft is unchanged."));
        if (m_composer->isInputVisible()) m_composer->editor()->setFocus(Qt::OtherFocusReason);
        renderDetails();
    });
    connect(&m_client,&HgsClient::sessionActionFinished,this,[this](quint64 id,bool ok,const QJsonObject &receipt,const QString &error) {
        for (const auto &key : m_compacts.keys()) {
            auto &pending = m_compacts[key];
            if (!pending.request || id != pending.request) continue;
            pending.request = 0;
            if (!ok) cancelCompactContinuation(key, error);
            else { pending.commandId=receipt.value("request_id").toString(); pollCompactContinuations(); }
            renderDetails(); return;
        }
    });
    m_compactTimer.setInterval(2500);
    connect(&m_compactTimer, &QTimer::timeout, this, &SessionsWindow::pollCompactContinuations);
    m_markRead=new ToolbarChip;m_markRead->setObjectName("activityMarkRead");
    m_markRead->setLabels(tr("Mark as read"));m_markRead->setIconName("read-all");m_markRead->setTone(ChipTone::Accent);
    toolbar->add(ComposerToolbar::Slot::MarkRead,m_markRead);
    m_activityView->useCompactJumpButton();toolbar->add(ComposerToolbar::Slot::Latest,m_activityView->jumpButton());
    auto *compaction=m_activityView->compactionIndicator();compaction->layout()->setContentsMargins(0,0,0,0);
    toolbar->add(ComposerToolbar::Slot::Compaction,compaction);toolbar->add(ComposerToolbar::Slot::CompactionCancel,m_compactCancel);
    m_cacheStatus=new CacheStatus::Chip;
    auto *cachePanel=new QWidget;cachePanel->setObjectName("cachePopover");
    auto *cacheLayout=new QVBoxLayout(cachePanel);cacheLayout->setContentsMargins(14,12,14,12);cacheLayout->setSpacing(8);
    m_cacheWarning=new QLabel;m_cacheWarning->setObjectName("cacheWarning");m_cacheWarning->setWordWrap(true);m_cacheWarning->setTextFormat(Qt::PlainText);
    m_cacheDetail=new QLabel;m_cacheDetail->setObjectName("cacheDetail");m_cacheDetail->setWordWrap(true);m_cacheDetail->setTextFormat(Qt::PlainText);
    auto *cacheActions=new QHBoxLayout;cacheActions->setSpacing(8);cacheActions->addStretch();
    auto *cacheUsage=new QPushButton(tr("Usage details"));cacheUsage->setObjectName("cacheUsageDetails");cacheUsage->setAutoDefault(false);
    m_cacheClear=new QPushButton(tr("Clear context"));m_cacheClear->setObjectName("cacheClearContext");m_cacheClear->setAutoDefault(false);
    m_cacheClear->setToolTip(tr("Clear this agent’s conversation, like /clear in Terminal. Your unsent message stays here."));
    cacheActions->addWidget(cacheUsage);cacheActions->addWidget(m_cacheClear);
    cacheLayout->addWidget(m_cacheWarning);cacheLayout->addWidget(m_cacheDetail);cacheLayout->addLayout(cacheActions);
    m_cacheStatus->setPopoverContent(cachePanel);
    toolbar->add(ComposerToolbar::Slot::Cache,m_cacheStatus);toolbar->add(ComposerToolbar::Slot::Context,m_contextUsage);
    connect(cacheUsage,&QPushButton::clicked,this,[this]{m_cacheStatus->closePopover();setInspectorVisible(true);m_inspector->setCurrentIndex(1);});
    // Close first: the confirmation dialog must not open over a popup.
    connect(m_cacheClear,&QPushButton::clicked,this,[this]{m_cacheStatus->closePopover();clearContext();});
    connect(&m_client,&HgsClient::sessionActionFinished,this,[this](quint64 id,bool ok,const QJsonObject &receipt,const QString &error) {
        if(id!=m_clearRequest)return;
        m_clearRequest=0;
        if(m_selectedKey==m_clearKey) {
            showNotice(ok?(receipt.value("status")=="confirmed"?tr("Context cleared. Your draft is ready to send."):tr("Clear command sent. Waiting for the agent to confirm the new conversation…")):error,!ok);
            inspect();renderDetails();
        }
        emit refreshRequested();
    });
    inputColumn->addWidget(m_composer);
    connect(m_composer, &MessageComposer::sessionRefreshRequested, this, [this](const QString &key) {
        if (key != m_selectedKey) return;
        emit refreshRequested(); inspect();
    });
    connect(m_markRead,&QPushButton::clicked,this,[this]{
        const auto *entry=selected();if(!entry||entry->session.state=="archived"||!entryNeedsAttention(*entry))return;
        const auto original=*entry;
        if(m_markRead->hasFocus())m_activityView->browser()->setFocus(Qt::OtherFocusReason);
        runSessionMenuAction(original,SessionMenuAction::MarkRead);
    });
    m_activityStack = new QStackedWidget; m_activityStack->setObjectName("activityStack");
    m_activityStack->addWidget(activityPage);
    auto *subagentPage = new QWidget; auto *subagentLayout = new QVBoxLayout(subagentPage);
    subagentLayout->setContentsMargins(0, 8, 0, 0); subagentLayout->setSpacing(8);
    auto *subagentTop = activityColumn(subagentLayout); subagentTop->setSpacing(8);
    auto *subagentHeader = new QHBoxLayout;
    auto *back = new QPushButton(tr("← Main activity")); back->setObjectName("subagentBack");
    connect(back, &QPushButton::clicked, this, &SessionsWindow::closeSubagent);
    subagentHeader->addWidget(back);
    m_subagentTitle = new QLabel; m_subagentTitle->setTextFormat(Qt::PlainText);
    m_subagentTitle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    subagentHeader->addWidget(m_subagentTitle, 1); subagentTop->addLayout(subagentHeader);
    m_subagentHint = new QLabel; m_subagentHint->setTextFormat(Qt::PlainText); m_subagentHint->setWordWrap(true);
    subagentTop->addWidget(m_subagentHint);
    m_subagentView = new ActivityView; m_subagentView->setObjectName("subagentActivity"); m_subagentView->browser()->setObjectName("subagentJournal"); subagentLayout->addWidget(m_subagentView, 1);
    m_subagentComposer = new MessageComposer; m_subagentComposer->setObjectName("subagentComposer"); m_subagentComposer->setInputVisible(false);
    // Voice delivery is scoped to a main conversation until the durable voice
    // protocol supports an exact child identity; never route a child to its parent.
    m_subagentContextUsage=new SessionUsage::ContextButton;m_subagentContextUsage->setObjectName("subagentContext");
    m_subagentComposer->toolbar()->add(ComposerToolbar::Slot::Context,m_subagentContextUsage);
    m_subagentView->useCompactJumpButton();m_subagentComposer->toolbar()->add(ComposerToolbar::Slot::Latest,m_subagentView->jumpButton());
    activityColumn(subagentLayout)->addWidget(m_subagentComposer);
    for(auto *context:{m_contextUsage,m_subagentContextUsage})connect(context,&QPushButton::clicked,this,[this]{setInspectorVisible(true);m_inspector->setCurrentIndex(1);});
    connect(m_subagentComposer, &MessageComposer::sendRequested, this,
        [this](const QString &key, const QString &text, const QList<MessageAttachment> &files) {
            const auto *entry = selected();
            if (!entry || m_subagentId.isEmpty() || key != childKey(*entry, m_subagentId)
                || !entry->online || !m_subagentDetails.value("send_supported").toBool() || m_subagentComposer->isSending(key)) return;
            const Entry target = *entry;
            const QString run = m_details.value("run_id").toString(), parentConversation = m_details.value("conversation_id").toString();
            const QString conversation = m_subagentConversation, agent = m_subagentId;
            beginSendEvidence(target.key, m_details);
            m_subagentComposer->prepareSendingAsync(key, false, [this, target, key, text, files, run, parentConversation, conversation, agent](bool okay) {
                if (!okay) { finishSendEvidence(target.key); return; }
                const auto current = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &value) { return value.key == target.key; });
                const auto evidence = m_pendingSendEvidence.value(target.key);
                finishSendEvidence(target.key);
                const bool inspectingChild = m_selectedKey == target.key && m_subagentId == agent;
                if (current == m_entries.cend() || current->host != target.host || current->session.name != target.session.name
                    || current->session.created != target.session.created
                    || !observedIdentityMatches(current->session, target.session, run, parentConversation) || !current->online || isTerminating(*current)
                    || m_pending || !m_renameKey.isEmpty()
                    || (!evidence.isEmpty() && (evidence.value("run_id").toString() != run || evidence.value("runtime_state") != "live" || evidence.value("process_state") != "running"))
                    || (inspectingChild && (m_subagentConversation != conversation || !m_subagentDetails.value("send_supported").toBool()))) {
                    m_subagentComposer->deliveryFinished(key, false, tr("The agent changed. Your draft was kept without sending.")); return;
                }
                const auto request = m_client.requestSendMessage(target.host, target.session.name, text, files, run, conversation, agent);
                m_childMessages.insert(request, key);
            });
        });
    connect(m_subagentView, &ActivityView::fileReferenceActivated, this, openFileReference);
    auto *attachments=new AttachmentLoader(&m_client,this);
    for(auto *view:{m_activityView,m_subagentView}) {
        connect(view,&ActivityView::attachmentActivated,this,[this,view,attachments](const QJsonObject &file){
            const auto *entry=selected();if(!entry)return;
            showNotice(tr("Opening attachment…"));
            attachments->open(entry->host,entry->session.name,m_details.value("conversation_id").toString(),
                entry->session.archiveId,view==m_subagentView?m_subagentId:QString(),file,
                [this](const QString &,const QString &error,const QString &notice){
                    // A plain success leaves "Opening attachment…" to expire on its own.
                    if(!error.isEmpty())showNotice(error,true);else if(!notice.isEmpty())showNotice(notice);
                });
        });
        connect(view,&ActivityView::voicePlaybackRequested,this,[this,view,attachments](const QString &key,const QJsonObject &file){
            const auto *entry=selected();if(!entry)return;
            attachments->fetch(entry->host,entry->session.name,m_details.value("conversation_id").toString(),entry->session.archiveId,
                view==m_subagentView?m_subagentId:QString(),file,[view,key](const QString &path,const QString &error){view->playVoice(key,path,error);},true);
        });
        connect(view,&ActivityView::attachmentPreviewRequested,this,[this,view,attachments](const QString &key,const QJsonObject &file){
            const auto *entry=selected();if(!entry)return;
            if(view==m_subagentView && m_subagentId.isEmpty())return;
            attachments->fetch(entry->host,entry->session.name,m_details.value("conversation_id").toString(),
                entry->session.archiveId,view==m_subagentView?m_subagentId:QString(),file,
                [view,key](const QString &path,const QString &error){
                    if(error.isEmpty())view->setAttachmentPreview(key,AttachmentLoader::thumbnail(path));
                });
        });
    }
    m_activityStack->addWidget(subagentPage);
    for (auto *view : {m_activityView, m_subagentView}) {
        // Activity's column edges and menu change the preference for every column.
        const auto changed = [this] { applyActivityWidth(); m_settingsPage->syncActivityWidth(); };
        connect(view, &ActivityView::columnWidthRequested, this, [changed](int width) {
            ActivityWidth::setWidth(qRound(width / ContentScale::factor())); ActivityWidth::setFullWidth(false); changed();
        });
        connect(view, &ActivityView::columnResetRequested, this, [changed] {
            ActivityWidth::setWidth(ActivityWidth::Default); ActivityWidth::setFullWidth(false); changed();
        });
        connect(view, &ActivityView::fullWidthRequested, this, [changed](bool full) { ActivityWidth::setFullWidth(full); changed(); });
    }
    m_question->hide();
    connect(m_question, &QuestionCard::answerRequested, this, &SessionsWindow::answerQuestion);
    connect(m_question, &QuestionCard::fileReferenceActivated, this, openFileReference);
    connect(m_question, &QuestionCard::queueNavigationRequested, this, [this](int direction) {
        if(const auto *entry=selected())renderQuestion(*entry,direction);
    });
    connect(m_question, &QuestionCard::openTerminalRequested, this, [this] {
        const auto *entry=selected();if(!entry)return;
        if(entry->session.cmd=="dsh") {showNativeUi(*entry);return;}
        if(!NativeSessionCapabilities::terminal(entry->session,m_details)){showNotice(NativeSessionCapabilities::terminalReason(entry->session,m_details),true);return;}
        m_detailTabs->setCurrentWidget(m_terminal); m_terminal->connectSession();
    });
    connect(&m_client, &HgsClient::nativeSessionLaunched, this, [this](const QString &id, bool ok, const QString &error) {
        // A listing can report the launch ended before its command returns; its
        // notice stays and nothing waits to open a session that never appears.
        if (ok) {
            if (!takeEndedLaunch(id)) { m_nativeLaunchToOpen=id; showNotice(tr("Session started. Loading activity…")); }
        } else {
            newSessionLaunchFailed(id, error);
        }
        emit refreshRequested();
    });
    connect(&m_client, &HgsClient::questionAnswered, this, [this](quint64 request, const QString &, const QString &name, const QJsonObject &receipt) {
        const auto pending = m_pendingAnswers.value(request);
        // Hook review and Codex's native installer continue in Terminal.
        const bool openTerminal = !pending.key.isEmpty() && pending.key == m_selectedKey
            && (pending.questionId.startsWith("codex-hooks-trust:") || pending.questionId.startsWith("codex-update:"))
            && pending.questionHash == receipt.value("question_hash").toString()
            && m_details.value("run_id") == receipt.value("run_id")
            && receipt.value("open_terminal").toBool();
        // After Update now, the receipt says whether Codex still runs its
        // installer or has exited; older hosts omit it.
        QString updateNotice;
        if (!pending.key.isEmpty() && pending.questionId.startsWith("codex-update:")
            && pending.questionHash == receipt.value("question_hash").toString()) {
            const auto entry = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &value) { return value.key == pending.key; });
            const QString target = entry == m_entries.cend() ? name : tr("%1 (%2)").arg(entry->session.name, entry->machine);
            const QString phase = receipt.value("codex_update").toString();
            if (phase == "installing")
                updateNotice = tr("Codex in %1 is installing its update in Terminal. Resume the session after Codex exits.").arg(target);
            else if (phase == "exited")
                updateNotice = tr("Codex in %1 exited after its update. Check the result in Terminal, then resume the session.").arg(target);
        }
        finishQuestion(request, true);
        if (openTerminal) { m_detailTabs->setCurrentWidget(m_terminal); m_terminal->connectSession(); }
        if (!updateNotice.isEmpty()) showNotice(updateNotice);
    });
    connect(&m_client, &HgsClient::questionAnswerFailed, this, [this](quint64 request, const QString &, const QString &, const QString &error, bool uncertain) {
        finishQuestion(request, false, error, uncertain);
    });
    connect(&m_client, &HgsClient::questionAnswerSubmitted, this, [this](quint64 request, const QString &, const QString &, const QJsonObject &) {
        finishQuestion(request, true, {}, false, true);
    });
    connect(m_composer,&MessageComposer::interruptRequested,this,[this](const QString &key) {
        const auto *entry=selected();
        if(!entry||key!=m_selectedKey||m_interruptRequest||m_details.value("interrupt_supported")!=true)return;
        cancelCompactContinuation(m_selectedKey, tr("Interrupted. Your draft was kept without sending."));
        if (m_startSend.key == m_selectedKey) cancelStartContinuation(tr("Interrupted. Your draft was kept without sending."));
        m_interruptKey=key;
        m_interruptName=entry->session.name; m_interruptMachine=entry->machine;
        // As in Terminal, the prompt of an interrupted turn returns to the message field.
        const QString prompt=interruptedPrompt(key);
        if(prompt.isEmpty())m_interruptPrompts.remove(key);
        else m_interruptPrompts.insert(key,{m_details.value("run_id").toString(),m_details.value("conversation_id").toString(),prompt});
        m_interruptRequest=m_client.requestInterrupt(entry->host,entry->session.name,m_details.value("run_id").toString(),
            m_details.value("conversation_id").toString(),m_details.value("turn_started").toDouble());
        renderDetails();
    });
    connect(&m_client,&HgsClient::sessionActionFinished,this,[this](quint64 id,bool ok,const QJsonObject &,const QString &error) {
        if(id!=m_interruptRequest)return;
        m_interruptRequest=0;
        if(!ok)m_interruptPrompts.remove(m_interruptKey);
        const auto target = tr("%1 (%2)").arg(m_interruptName, m_interruptMachine);
        const bool alreadyStopped = error.trimmed() == "hgs: This turn is no longer working"
            || error.trimmed() == "This turn is no longer working";
        if (ok) showNotice(tr("Interrupt sent to %1. Waiting for the agent to stop…").arg(target));
        else if (alreadyStopped) showNotice(tr("Stop %1: the turn already ended or is waiting for input. No action needed.").arg(target));
        else showNotice(tr("Stop %1 failed: %2\nRefresh status or open Terminal to check the agent before trying again.").arg(target, error), true);
        if(m_selectedKey==m_interruptKey)inspect();
        emit refreshRequested();
        renderDetails();
    });
    connect(m_question, &QuestionCard::draftRestored, this, [this] { renderDetails(); });
    connect(m_composer, &MessageComposer::sendRequested, this, [this](const QString &key,const QString &text,const QList<MessageAttachment> &files){sendMessage(key,text,files);});
    // Send and push now: only this field's messages enter the session's native queue.
    m_composer->setPushEnabled(true);
    connect(m_composer, &MessageComposer::pushRequested, this, &SessionsWindow::armPush);
    connect(m_composer, &MessageComposer::sendNowRequested, this, [this] { sendQueuedNow(); });
    // An unsent draft becomes the row's status (see SessionDelegate::statusOf).
    const auto draftChanged = [this](const QString &key) {
        for (int row = 0; row < m_sessions->count(); ++row) {
            auto *item = m_sessions->item(row);
            if (item->data(SessionRoles::Key).toString() == key && item->data(SessionRoles::ChildId).toString().isEmpty())
                item->setData(SessionRoles::Draft, m_composer->hasDraft(key));
        }
        // Presence changes once when typing starts or the draft clears, rather
        // than on each keystroke. Defer list rebuilding until the editor returns.
        if (m_draftRefreshPending) return;
        m_draftRefreshPending = true;
        QTimer::singleShot(0, this, [this] {
            m_draftRefreshPending = false;
            if (isVisible()) rebuild();
            updateDashboard();
        });
    };
    connect(m_composer, &MessageComposer::draftChanged, this, draftChanged);
    connect(m_subagentComposer, &MessageComposer::draftChanged, this, draftChanged);
    for (auto *composer : {m_composer, m_subagentComposer}) {
        composer->canOpenDraftSession = [this](const QString &key) { return draftOwner(key) != nullptr; };
        composer->savedDraftSessionRoles = [this](const QString &key) {
            QMap<int,QVariant> roles;
            const auto *entry=draftOwner(key);if(!entry)return roles;
            const auto &session=entry->session;
            roles={{SessionRoles::Title,sessionLabel(session)},{SessionRoles::Agent,session.cmd},
                {SessionRoles::Host,entry->machine},{SessionRoles::MachineName,entry->machine},
                {SessionRoles::MachineColor,MachineAppearance::color(entry->machine)},
                {SessionRoles::Meta,SessionPresentation::checkoutLine(session)},
                {SessionRoles::MetaIcon,SessionPresentation::checkoutIcon(session)}};
            if(ComposerDraftStore::owner(key).kind!=ComposerDraftOwner::Kind::Subagent){roles[SessionRoles::Model]=session.model;roles[SessionRoles::Effort]=session.effort;}
            return roles;
        };
        connect(composer, &MessageComposer::draftSessionRequested, this, &SessionsWindow::openDraftSession);
    }
    connect(m_activityView, &ActivityView::messageActionRequested, this, &SessionsWindow::messageAction);
    connect(m_composer, &MessageComposer::settingsTerminalRequested, this, [this](const QString &key) {
        if (key != m_selectedKey || !selected()) return;
        m_detailTabs->setCurrentWidget(m_terminal); m_terminal->connectSession();
    });
    connect(m_composer, &MessageComposer::settingsRequested, this, &SessionsWindow::changeModelSettings);
    connect(&m_client, &HgsClient::settingsFinished, this, [this](quint64 request, bool ok, const QJsonObject &receipt, const QString &error) {
        if (request != m_settingsRequest) return;
        m_settingsRequest = 0;
        // Dispatch schedules an automatic apply when the session left Ready
        // after the idle listing, as for a chooser that appeared meanwhile. It
        // stays queued for one more attempt after the next Ready transition;
        // a failure, a refusal and an applied change end automatic attempts.
        const bool scheduledAgain = ok && receipt.value("status") == "scheduled" && m_settingsAutomatic;
        if (scheduledAgain) m_scheduledSettings[m_settingsKey] = {m_settingsIdentity};
        else m_scheduledSettings.remove(m_settingsKey);
        if (ok && receipt.value("status") == "scheduled" && !m_settingsAutomatic) {
            m_queuedSettings[m_settingsKey] = {m_settingsHost, m_settingsName, m_settingsRun, m_settingsConversation,
                receipt.value("model").toString(), receipt.value("effort").toString(), receipt.value("pending_settings_id").toString(receipt.value("request_id").toString())};
        } else if (!scheduledAgain) m_queuedSettings.remove(m_settingsKey);
        m_settingsIdentity.clear();
        if (m_selectedKey == m_settingsKey && m_details.value("run_id").toString() == m_settingsRun
            && m_details.value("conversation_id").toString() == m_settingsConversation && ok) {
            const bool scheduled = receipt.value("status") == "scheduled";
            if (scheduled) {
                m_details["pending_model"] = receipt.value("model"); m_details["pending_effort"] = receipt.value("effort");
                m_details["pending_settings_id"] = receipt.value("pending_settings_id").toString(receipt.value("request_id").toString());
            } else {
                m_details["model"] = receipt.value("model"); m_details["effort"] = receipt.value("effort");
                m_details.remove("pending_model"); m_details.remove("pending_effort"); m_details.remove("pending_settings_id");
            }
            // inspect() below starts nothing while one runs, and that one read
            // the session before this change.
            if (const auto *entry = selected();
                entry && m_client.inspectionInFlight(entry->host, entry->session.name, entry->session.archiveId))
                m_inspectionBehindSettings.insert(m_settingsKey);
        }
        // A send or another client can consume/replace the pending request.
        // These exact preflight refusals send no native input; refresh below
        // without reporting a failed mutation or retrying the stale request.
        const QString detail = error.startsWith("hgs: ") ? error.mid(5) : error;
        const bool stalePending = m_settingsAutomatic &&
            (detail == "pending settings changed; refresh before applying"
             || detail == "pending settings target changed; refresh before applying");
        if (!ok && !stalePending) showNotice(tr("Model settings for %1: %2").arg(m_settingsName, error), true);
        m_settingsKey.clear(); m_settingsRun.clear(); m_settingsConversation.clear();
        renderDetails(); emit refreshRequested(); inspect();
    });
    connect(&m_client, &HgsClient::messageSent, this, [this](quint64 request, const QString &, const QString &, const QJsonObject &receipt) {
        finishMessage(request, true, receipt);
    });
    connect(&m_client, &HgsClient::messageFailed, this, [this](quint64 request, const QString &, const QString &, const QString &error, bool uncertain) {
        finishMessage(request, false, {}, error, uncertain);
    });
    m_terminal = new TerminalView;
    m_children = browser(); m_children->setObjectName("subagents"); m_children->setAccessibleName(tr("Subagents")); m_children->viewport()->installEventFilter(this);
    connect(m_children, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        if (url.scheme() == "hgs-agent") openSubagent(url.path(QUrl::FullyDecoded));
    });
    m_info = browser(); m_info->setObjectName("sessionInfo"); m_info->setAccessibleName(tr("Session details"));
    tabs->addTab(m_activityStack, tr("Activity")); tabs->addTab(m_terminal, tr("Terminal"));
    m_nativeUi = new NativeUiView(&m_client, [] { return NativeBrowserPlugin::factory(); });
    tabs->addTab(m_nativeUi, tr("Native UI")); tabs->setTabVisible(2, false);
    m_processes=new ProcessView(&m_client);tabs->addTab(m_processes,tr("Processes"));tabs->setTabVisible(tabs->indexOf(m_processes),ProcessSettings::enabled());
    connect(m_processes,&ProcessView::refreshRequested,this,&SessionsWindow::inspect);
    for(auto *view:{m_activityView,m_subagentView})connect(view,&ActivityView::processRequested,this,[this](const QString &id){if(!ProcessSettings::enabled())return;m_detailTabs->setCurrentWidget(m_processes);m_processes->selectProcess(id);});
    connect(m_nativeUi, &NativeUiView::responseRequested, this, [this] { closeSubagent(); m_detailTabs->setCurrentWidget(m_activityStack); });
    m_workSplitter = new QSplitter(Qt::Horizontal); m_workSplitter->setObjectName("sessionWorkSplitter");
    m_workSplitter->setHandleWidth(8);
    m_workSplitter->setChildrenCollapsible(false);
    m_workSplitter->addWidget(tabs); tabs->setMinimumWidth(280);
    m_inspectorPanel = new QFrame; m_inspectorPanel->setObjectName("sessionInspectorPanel");
    auto *inspectorLayout = new QVBoxLayout(m_inspectorPanel);
    inspectorLayout->setContentsMargins(8, 4, 8, 8); inspectorLayout->setSpacing(0);
    m_inspector = new QTabWidget; m_inspector->setObjectName("sessionInspector");
    m_inspector->setDocumentMode(true); m_inspector->tabBar()->setDrawBase(false);
    // Include full native-font labels in the inspector's minimum size.
    m_inspector->tabBar()->setElideMode(Qt::ElideNone);
    m_inspector->tabBar()->setUsesScrollButtons(false);
    m_inspector->tabBar()->installEventFilter(this);
    m_worktrees=new WorktreePanel(&m_client);
    m_worktrees->filterRequested=[this](const QString &path,bool checkout){if(const auto *entry=selected())filterFolder(entry->host,path,checkout);};
    m_worktrees->sessionRequested=[this](const QString &name,const QString &archive){if(const auto *entry=selected())openRelatedSession(entry->host,name,archive);};
    m_worktrees->launchRequested=[this](const QString &path){if(const auto *entry=selected())showNewSession({},m_organization.groupFor(entry->identity),entry->host,{},path);};
    auto *detailsPage=new QWidget;auto *detailsLayout=new QVBoxLayout(detailsPage);detailsLayout->setContentsMargins(0,0,0,0);
    auto *identity = new QWidget; identity->setObjectName("detailsIdentity");
    auto *identityLayout = new QVBoxLayout(identity); identityLayout->setContentsMargins(4,8,4,2); identityLayout->setSpacing(4);
    m_detailsIdTitle = label(tr("Session ID"), "detailsIdentityTitle"); identityLayout->addWidget(m_detailsIdTitle);
    auto *identityRow = new QHBoxLayout;
    m_detailsIdValue = new SessionIdentityLabel(tr("Not confirmed"));
    m_detailsIdValue->setObjectName("detailsIdentityValue"); m_detailsIdValue->setTextFormat(Qt::PlainText);
    m_detailsIdValue->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred); m_detailsIdValue->setMinimumWidth(0);
    m_detailsIdValue->setWordWrap(true);
    m_detailsIdValue->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    identityRow->addWidget(m_detailsIdValue);
    m_copySessionId = iconButton("copy", tr("Copy session ID"), "copySessionIdAction"); m_copySessionId->setAutoDefault(false);
    m_copySessionId->setProperty("sessionHeaderAction", true);
    configureWorkspaceIconButton(m_copySessionId, "copy", tr("Copy session ID"), m_dark);
    connect(m_copySessionId, &QPushButton::clicked, this, [this] {
        const auto value = m_detailsIdValue->property("copyValue").toString();
        if (!selected() || value.isEmpty()) return;
        emit copyTextRequested(value); showNotice(tr("%1 copied").arg(m_detailsIdTitle->text()));
    });
    identityRow->addWidget(m_copySessionId); identityRow->addStretch(); identityLayout->addLayout(identityRow);
    detailsLayout->addWidget(identity); detailsLayout->addWidget(m_info,1);
    auto *contextRow=new QHBoxLayout;m_detailsContext=new QLabel;m_detailsContext->setObjectName("detailsContext");m_detailsContext->setWordWrap(true);
    m_detailsClear=new QPushButton(tr("Clear session"));m_detailsClear->setObjectName("clearSessionFromDetails");m_detailsClear->setAutoDefault(false);
    m_detailsClear->setToolTip(tr("Start this agent's conversation from scratch, like /clear in Terminal. Your unsent draft is kept."));
    connect(m_detailsClear,&QPushButton::clicked,this,&SessionsWindow::clearContext);
    contextRow->addWidget(m_detailsContext,1);contextRow->addWidget(m_detailsClear);detailsLayout->addLayout(contextRow);
    m_inspector->addTab(m_children, tr("Tasks && agents")); m_inspector->addTab(detailsPage, tr("Details"));
    m_inspector->addTab(m_worktrees, tr("Worktrees"));
    connect(m_inspector,&QTabWidget::currentChanged,this,[this]{updateWorktrees();});
    inspectorLayout->addWidget(m_inspector);
    m_workSplitter->addWidget(m_inspectorPanel);
    m_workSplitter->setStretchFactor(0, 1); m_workSplitter->setStretchFactor(1, 0);
    m_inspectorToggle = new QPushButton; m_inspectorToggle->setObjectName("toggleInspector");
    m_inspectorToggle->setProperty("glyph", "inspector"); m_inspectorToggle->setCheckable(true);
    m_inspectorToggle->setProperty("sessionHeaderAction", true);
    configureWorkspaceIconButton(m_inspectorToggle, "inspector", tr("Show tasks, agents, details and worktrees"), m_dark);
    tabs->setCornerWidget(m_inspectorToggle);
    connect(m_inspectorToggle, &QPushButton::toggled, this, &SessionsWindow::setInspectorVisible);
    connect(m_inspector, &QTabWidget::currentChanged, this, [](int index) { QSettings().setValue("workspace/inspectorTab", index); });
    connect(m_workSplitter, &QSplitter::splitterMoved, this, [this] {
        if (!m_inspectorPanel->isHidden()) { m_inspectorWidth = m_inspectorPanel->width(); QSettings().setValue("workspace/inspectorWidth", m_inspectorWidth); }
    });
    m_inspector->setCurrentIndex(qBound(0, settings.value("workspace/inspectorTab", 0).toInt(), m_inspector->count() - 1));
    m_inspectorWidth = qBound(240, settings.value("workspace/inspectorWidth", 290).toInt(), 600);
    m_inspectorPanel->hide();
    if (settings.value("workspace/inspectorVisible", false).toBool())
        QTimer::singleShot(0, this, [this] { setInspectorVisible(true); });
    connect(tabs, &QTabWidget::currentChanged, this, [this](int) {
        updateSessionFeedback();
        if (m_detailTabs->currentWidget() == m_terminal) m_terminal->connectSession();
        if (m_detailTabs->currentWidget() == m_processes) m_processPollAge.invalidate();
        if(m_detailTabs->currentWidget()!=m_terminal && m_detailTabs->currentWidget()!=m_nativeUi)inspect();
    });
    detail->addWidget(m_workSplitter, 1); m_detailStack->addWidget(details); m_splitter->addWidget(m_detailStack);
    m_fileDrop=new SessionFileDrop(details,[this]()->MessageComposer *{
        if(!selected()||m_detailTabs->currentWidget()==m_terminal)return nullptr;
        if(m_detailTabs->currentIndex()==0&&!m_subagentId.isEmpty())return m_subagentComposer->isInputVisible()?m_subagentComposer:nullptr;
        return m_composer;
    },[this](MessageComposer *composer){if(composer==m_composer)closeSubagent();m_detailTabs->setCurrentIndex(0);},
    [this]{return terminalDropTarget();},[this](const QStringList &paths){dropTerminalFiles(paths);});
    connect(&m_client,&HgsClient::sessionActionFinished,this,[this](quint64 id,bool ok,const QJsonObject &result,const QString &error) {
        if(id!=m_terminalDropRequest)return;
        m_terminalDropRequest=0;
        // An hgs that predates untracked drops asks for a saved binding.
        if(!ok&&!m_terminalDropUntracked.isEmpty()&&error.contains("no saved binding")) {
            showNotice(tr("Update hgs on %1 to drop files into an untracked session.").arg(m_terminalDropUntracked),true);return;
        }
        if(!ok){showNotice(error,true);return;}
        if(terminalDropTarget()!=m_terminalDropKey) {showNotice(tr("Terminal changed during file transfer. Drop the files again in the intended session."),true);return;}
        QStringList paths;
        for(const auto &file:result.value("files").toArray()) {
            const auto path=file.toObject().value("path").toString();
            if(!QDir::isAbsolutePath(path)||path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))) {showNotice(tr("Invalid transferred file path"),true);return;}
            paths<<path;
        }
        if(paths.size()!=m_terminalDropCount){showNotice(tr("Incomplete file transfer"),true);return;}
        pasteTerminalPaths(paths);
    });
    m_splitter->setStretchFactor(0, 0); m_splitter->setStretchFactor(1, 1); m_splitter->setSizes({310, 814});
    if (!settings.value("workspace/splitter").toByteArray().isEmpty()) m_splitter->restoreState(settings.value("workspace/splitter").toByteArray());
    layout->addWidget(m_splitter, 1);
    m_sessionDock = new SessionPanelDock(m_splitter, sessionSlot, listPanel, m_sessions, this);
    m_sessionDock->layoutChanged = [this] { applySessionStrip(); };
    m_sessionDock->keepPeek = [this] { return m_search->hasFocus() || !m_search->text().isEmpty(); };
    m_sessionDock->modeChanged = [this] {
        const auto mode = m_sessionDock->mode();
        if (mode != SessionPanelDock::Peek) QSettings().setValue("workspace/sessionsCollapsed", mode == SessionPanelDock::Collapsed);
        if (mode != SessionPanelDock::Docked && m_sessions->bulkSelecting()) m_sessions->clearBulkSelection();
        updateSessionsToggle(); QTimer::singleShot(0, this, &SessionsWindow::updateInspectorMinimum);
    };
    m_sessionDock->setHoverExpands(settings.value("workspace/expandSessionsOnHover", true).toBool());
    m_sessionDock->restore(settings.value("workspace/sessionsCollapsed", false).toBool(), settings.value("workspace/sessionsWidth", 0).toInt());
    connect(m_sessionsToggle, &QPushButton::clicked, this, [this] { m_sessionDock->toggle(); });
    m_pages = new QStackedWidget; m_pages->addWidget(body);
    connect(m_pages, &QStackedWidget::currentChanged, this, [this](int index) {
        if (index != 0) m_terminal->disconnectSession();
    });
    auto *projectClient = new HgsClient(hgsPath, this);
    projectClient->setObjectName("projectClient");
    m_projectsPage = new ProjectsDialog(projectClient, this, true, &m_organization); m_pages->addWidget(m_projectsPage);
    connect(m_projectsPage, &ProjectsDialog::projectsChanged, this, &SessionsWindow::projectsChanged);
    connect(m_projectsPage, &ProjectsDialog::organizationChanged, this, [this] { saveOrganization(); rebuild(); });
    m_swarm=new SwarmController(projectClient,this);
    connect(m_projectsPage,&ProjectsDialog::catalogDraftEditing,m_swarm,&SwarmController::setDraftEditing);
    connect(m_swarm,&SwarmController::organizationReady,this,[this](const QJsonObject &organization){
        const SessionOrganization updated(organization);
        if(updated.toJson()==m_organization.toJson())return;
        m_organization=updated;saveOrganization();m_projectsPage->refresh();rebuild();
    });
    connect(m_swarm,&SwarmController::statusChanged,m_projectsPage,&ProjectsDialog::setSwarmStatus);
    connect(m_projectsPage,&ProjectsDialog::swarmRequested,this,[this,projectClient]{
        showSwarmDialog(projectClient,m_swarm,m_fleet,this,[this]{m_pages->setCurrentIndex(2);});
    });
    QTimer::singleShot(0,this,[this]{m_swarm->start(m_organization.toJson());});
    connect(m_projectsPage,&ProjectsDialog::folderSessionsRequested,this,&SessionsWindow::filterFolder);
    connect(m_projectsPage,&ProjectsDialog::relatedSessionRequested,this,&SessionsWindow::openRelatedSession);
    connect(m_projectsPage,&ProjectsDialog::newPathSessionRequested,this,[this](const QString &project,const QString &host,const QString &path){showNewSession({},project,host,{},path);});
    connect(m_projectsPage,&ProjectsDialog::newSessionRequested,this,[this](const QString &project,const QString &host,const QString &folder){showNewSession({},project,host,folder);});
    m_machinesPage = new MachinesPage(hgsPath); m_pages->addWidget(m_machinesPage);
    connect(m_machinesPage, &MachinesPage::machinesChanged, this, &SessionsWindow::refreshRequested);
    connect(m_machinesPage, &MachinesPage::sshTerminalRequested, this, &SessionsWindow::machineSshRequested);
    connect(m_machinesPage, &MachinesPage::viewSessionsRequested, this, [this](const QString &host) {
        m_filter = "all"; m_folderFilterPath.clear(); m_projectFilters.clear(); m_hostFilters = {host.isEmpty() ? QStringLiteral("@local") : host}; m_search->clear(); showSessionList();
    });
    m_dashboard = new DashboardPage; m_pages->addWidget(m_dashboard);
    connect(m_dashboard,&DashboardPage::refreshRequested,this,[this]{emit refreshRequested();updateDashboardAccounts(true,true);});
    connect(m_dashboard,&DashboardPage::accountRequested,this,[this](const QString &host,const QString &id){m_pages->setCurrentIndex(4);m_accountsPage->showAccount(host,id);});
    connect(m_dashboard,&DashboardPage::accountsRequested,this,[this]{m_pages->setCurrentIndex(4);});
    connect(m_dashboard,&DashboardPage::sessionRequested,this,&SessionsWindow::showAttentionSession);
    connect(m_dashboard,&DashboardPage::machinesRequested,this,[this](const QString &host) { m_pages->setCurrentIndex(2); m_machinesPage->selectMachine(host); });
    connect(m_dashboard,&DashboardPage::filterRequested,this,[this](const QString &host,const QString &filter) {
        m_filter=filter; m_folderFilterPath.clear(); m_projectFilters.clear(); m_hostFilters = host == "@all" ? QSet<QString>() : QSet<QString>{host.isEmpty()?QStringLiteral("@local"):host}; m_search->clear(); showSessionList();
    });
    m_accountsPage = new AccountsPage(hgsPath,nullptr,m_usageStore); m_pages->addWidget(m_accountsPage);
    m_settingsPage=new SettingsPage(hgsPath);m_pages->addWidget(m_settingsPage);
    connect(m_swarm,&SwarmController::snapshotChanged,this,[this](const QJsonObject &snapshot){m_settingsPage->setSwarmSnapshot(snapshot);});
    m_settingsPage->pollingChanged=[this]{
        const bool enabled=ProcessSettings::enabled();
        m_processes->applyPreferences();m_processPollAge.invalidate();
        if(!enabled){m_details.remove("processes");m_subagentDetails.remove("processes");m_processes->setSession({},{},{},{},false,false);}
        if(!enabled && m_detailTabs->currentWidget()==m_processes)m_detailTabs->setCurrentIndex(0);
        m_detailTabs->setTabVisible(m_detailTabs->indexOf(m_processes),enabled);renderDetails();
    };
    m_settingsPage->appearanceChanged=[this]{
        m_sessions->setProperty("compact",QSettings().value("workspace/compact",false).toBool());m_sessions->doItemsLayout();
        m_sessionDock->setStripWidth(SessionStrip::width(m_sessions->property("compact").toBool()));
        m_sessionDock->setHoverExpands(QSettings().value("workspace/expandSessionsOnHover",true).toBool());
        const auto usageIndicator=AccountUsage::indicator();m_accountUsage->setIndicator(usageIndicator);m_dashboard->setUsageIndicator(usageIndicator);
        applyTheme();rebuild();
    };
    m_settingsPage->contentScaleChanged=[this]{applyContentScale();};
    m_settingsPage->activityWidthChanged=[this]{applyActivityWidth();};
    // The rail pin and Settings → Appearance switch the same preference.
    auto *windowLayer=new WindowLayer(this);
    const auto updateWindowLayer=[this,windowPin,windowLayer]{
        const QSignalBlocker block(windowPin);windowPin->setChecked(windowLayer->onTop());
        // Disabling a focused control moves focus to the next rail button.
        // WindowLayer already rejects duplicate requests while KWin is busy.
        windowPin->setVisible(windowLayer->supported());windowPin->setEnabled(windowLayer->supported());
        updateWindowPinAppearance();
        m_settingsPage->setWindowLayerState(windowLayer->onTop(),windowLayer->supported(),windowLayer->hint());
    };
    m_settingsPage->windowLayerChanged=[windowLayer](bool on){windowLayer->request(on);};
    connect(windowPin,&QPushButton::toggled,windowLayer,&WindowLayer::request);
    connect(windowLayer,&WindowLayer::changed,this,updateWindowLayer);
    updateWindowLayer();
    connect(m_accountsPage,&AccountsPage::loginRequested,this,&SessionsWindow::accountLoginRequested);
    connect(m_accountsPage,&AccountsPage::installRequested,this,&SessionsWindow::accountInstallRequested);
    connect(m_accountsPage,&AccountsPage::accountsChanged,this,&SessionsWindow::refreshRequested);
    connect(m_accountsPage,&AccountsPage::notice,this,&SessionsWindow::showNotice);
    connect(m_accountsPage,&AccountsPage::catalogChanged,this,[this]{updateDashboardAccounts(m_pages->currentIndex()==3);});
    connect(m_usageStore,&AccountUsageStore::changed,this,[this](const QString &){renderAccountUsage();updateDashboardAccounts();});
    connect(m_machinesPage,&MachinesPage::accountsRequested,this,[this](const QString &host) { m_pages->setCurrentIndex(4); m_accountsPage->showMachine(host); });
    connect(m_machinesPage,&MachinesPage::appearanceChanged,this,[this] { m_machineFilter->refreshColors(); m_accountsPage->refreshAppearance(); rebuild(); updateDashboard(); });
    connect(m_pages,&QStackedWidget::currentChanged,this,[this](int index) {
        m_sessionsNav->setChecked(index==0); m_projectsNav->setChecked(index==1); m_machinesNav->setChecked(index==2); m_brand->setChecked(index==3); m_accountsNav->setChecked(index==4); m_settingsNav->setChecked(index==5);
        renderAccountUsage();if(index==3)updateDashboardAccounts(true);updateInspectorMinimum();
        if (index == 3 && m_dashboardStale) updateDashboard();
        // Callers that show Sessions rebuild right away; this covers any other switch.
        if (index == 0 && m_sessionsStale) QTimer::singleShot(0, this, [this] { if (m_sessionsStale && m_pages->currentIndex() == 0) rebuild(); });
    });
    auto *workspace = new QVBoxLayout; workspace->setContentsMargins(0, 0, 0, 0); workspace->setSpacing(0);
    workspace->addWidget(m_pages, 1);
    auto *statusBar = new QFrame; statusBar->setObjectName("workspaceStatus"); statusBar->setFixedHeight(28);
    auto *statusLayout = new QHBoxLayout(statusBar); statusLayout->setContentsMargins(12, 3, 12, 3);
    m_connectionStatus = new StatusMessage; m_connectionStatus->setObjectName("workspaceConnectionStatus");
    statusLayout->addWidget(m_connectionStatus, 1);
    auto *updateEntry = new QPushButton(tr("New version"));
    updateEntry->setObjectName("updatesAvailable");
    updateEntry->setProperty("glyph", "zerus");
    updateEntry->setIconSize(QSize(14, 14));
    updateEntry->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    statusLayout->addWidget(updateEntry);
    auto *updates = UpdateController::instance();
    auto updateEntryState = [updates, updateEntry] {
        updateEntry->setVisible(updates->updateAvailable());
        const auto description = tr("Zerus %1 is available. Open Updates.").arg(updates->feed().version);
        updateEntry->setToolTip(description);
        updateEntry->setAccessibleName(description);
    };
    connect(updates, &UpdateController::changed, this, updateEntryState);
    connect(updateEntry, &QPushButton::clicked, this, &SessionsWindow::showUpdates);
    updateEntryState();
    auto *restartEntry = new QPushButton(tr("Restart to finish update"));
    restartEntry->setObjectName("restartToUpdate");
    restartEntry->setProperty("glyph", "zerus");
    restartEntry->setIconSize(QSize(14, 14));
    restartEntry->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    restartEntry->setToolTip(tr("Zerus was updated on disk. Drafts are kept; sessions keep running."));
    restartEntry->setAccessibleName(restartEntry->toolTip());
    statusLayout->addWidget(restartEntry);
    auto restartEntryState = [updates, restartEntry] { restartEntry->setVisible(updates->restartRequired()); };
    connect(updates, &UpdateController::changed, this, restartEntryState);
    connect(restartEntry, &QPushButton::clicked, updates, &UpdateController::restartIntoInstalledBundle);
    restartEntryState();
    m_connectionRetry = new QPushButton(tr("Retry now")); m_connectionRetry->setObjectName("connectionRetry");
    m_connectionRetry->setFlat(true); m_connectionRetry->hide(); statusLayout->addWidget(m_connectionRetry);
    connect(m_connectionRetry, &QPushButton::clicked, this, &SessionsWindow::refreshRequested);
    m_notice = new StatusMessage; m_notice->hide(); statusLayout->addWidget(m_notice, 1);
    m_sessionFeedback = new StatusMessage; m_sessionFeedback->setObjectName("sessionFeedback");
    m_sessionFeedback->hide(); statusLayout->addWidget(m_sessionFeedback, 1);
    connect(m_pages, &QStackedWidget::currentChanged, this, [this] { updateSessionFeedback(); });
    m_noticeDismiss = new QPushButton; m_noticeDismiss->setObjectName("dismissNotice");
    m_noticeDismiss->setProperty("glyph", "close"); m_noticeDismiss->setIconSize(QSize(14, 14));
    m_noticeDismiss->setFixedSize(20, 20); m_noticeDismiss->setFlat(true);
    m_noticeDismiss->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    m_noticeDismiss->setToolTip(tr("Dismiss notification")); m_noticeDismiss->setAccessibleName(m_noticeDismiss->toolTip());
    m_noticeDismiss->hide(); statusLayout->addWidget(m_noticeDismiss);
    connect(m_noticeDismiss, &QPushButton::clicked, this, &SessionsWindow::clearNotice);
    m_worktreeCleanup = new QPushButton(tr("Clean up worktree…")); m_worktreeCleanup->setObjectName("archivedWorktreeCleanup");
    m_worktreeCleanup->setFlat(true); m_worktreeCleanup->hide(); statusLayout->addWidget(m_worktreeCleanup);
    connect(m_worktreeCleanup, &QPushButton::clicked, this, &SessionsWindow::cleanUpArchivedWorktree);
    statusLayout->addWidget(m_usageStrip,0,Qt::AlignRight);
    workspace->addWidget(statusBar); root->addLayout(workspace, 1);

    // Search, refresh and the other commands are keyboard actions (setupShortcuts()).
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    // Escape never closes Zerus. A search being typed is cleared first;
    // otherwise, like Escape in Terminal, it stops the selected session's
    // working turn. Terminal and the message field handle their own Escape.
    connect(escape, &QShortcut::activated, this, [this] {
        if (m_sessionDock->dismissPeek()) m_search->clear();
        else if (m_search->hasFocus() && !m_search->text().isEmpty()) m_search->clear();
        else if (m_pages->currentIndex() == 0 && m_composer->requestInterrupt()) return;
        else if (!m_search->text().isEmpty()) m_search->clear();
    });
    connect(m_activityView, &ActivityView::queueSendNowRequested, this, [this](const QString &id) { if (!id.isEmpty()) sendQueuedNow(id); });
    connect(&m_client, &HgsClient::queueSendFinished, this, [this](quint64 request, bool ok, const QJsonObject &, const QString &error) {
        if (request != m_queueSendRequest) return;
        const bool current = m_queueSendKey == m_selectedKey; m_queueSendRequest = 0; m_queueSendKey.clear();
        if (current) { m_details.remove("input_queue"); showNotice(ok ? tr("Send now requested. Waiting for the agent to accept the queue.") : error, !ok); renderDetails(); inspect(); }
        emit refreshRequested();
    });
    connect(&m_client, &HgsClient::inspectionReady, this, &SessionsWindow::acceptInspection);
    connect(&m_client, &HgsClient::subagentInspectionReady, this,
        [this](const QString &host,const QString &name,const QString &id,const QString &archive,const QJsonObject &data) {
            const auto *entry = selected();
            if (!entry || entry->host != host || entry->session.name != name || entry->session.archiveId != archive
                || m_subagentId != id || m_subagentConversation != data.value("parent_conversation_id").toString()
                || m_details.value("run_id") != data.value("run_id")) return;
            m_subagentDetails = data;
            m_subagentCwd = data.value("cwd").toString();
            m_subagentHint->setText(data.value("history_scope").toString()
                + (data.value("history_truncated").toBool() ? tr(". Showing recent activity.") : QString())
                + (data.value("send_supported").toBool() ? tr(" Messages go directly to this agent.")
                    : tr(" Read only. This agent is managed by the parent conversation.")));
            m_subagentComposer->setInputVisible(data.value("send_supported").toBool());
            m_subagentComposer->setAvailability(!isTerminating(*entry) && entry->online && data.value("send_supported").toBool());
            m_subagentView->setActivity(data,data.value("events").toArray()); renderDetails();
        });
    connect(&m_client, &HgsClient::subagentInspectionFailed, this,
        [this](const QString &host,const QString &name,const QString &id,const QString &archive,const QString &error) {
            const auto *entry = selected();
            if (entry && entry->host == host && entry->session.name == name && entry->session.archiveId == archive && m_subagentId == id)
                m_subagentHint->setText(error);
        });
    connect(&m_client, &HgsClient::inspectionFailed, this, [this](const QString &host, const QString &name, const QString &error, const QString &archiveId) {
        if (!m_pending && !m_renameKey.isEmpty()) return;
        const auto *entry = selected(); if (!entry || entry->host != host || entry->session.name != name || entry->session.archiveId != archiveId) return;
        m_inspectError = tr("Could not refresh activity: %1").arg(error); renderDetails();
    });
    connect(&m_client, &HgsClient::writeDone, this, [this](const QString &operation, bool ok, const QString &detail) {
        if (finishSelectionAction(operation, ok, detail)) {if(!m_selectionRunning)m_archiveBrowser->refresh();return;}
        if(ok&&(operation.startsWith("archive ")||operation.startsWith("restore ")||operation.startsWith("forget archive ")||operation.startsWith("rename ")))m_archiveBrowser->refresh();
        if (m_archivedWorktree.size() == 4 && (operation == QStringLiteral("archive %1").arg(m_archivedWorktree[1]) || operation == QStringLiteral("terminate %1").arg(m_archivedWorktree[1]))) {
            if (ok) offerWorktreeCleanup(); else m_archivedWorktree.clear();
        }
        if (!ok && (operation == QStringLiteral("resume %1").arg(m_startSend.name) || operation == QStringLiteral("fresh %1").arg(m_startSend.name)))
            cancelStartContinuation(detail);
        if (!m_terminationCommand.isEmpty() && (operation == QStringLiteral("kill %1").arg(m_terminating.value(m_terminationCommand).session.name) || operation == QStringLiteral("terminate %1").arg(m_terminating.value(m_terminationCommand).session.name))) {
            if(ok)m_terminationFinished.insert(m_terminationCommand);
            else m_terminating.remove(m_terminationCommand);
            m_terminationCommand.clear();
        }
        m_pending = false;
        const bool renamed = !m_renameKey.isEmpty();
        if (!m_renameKey.isEmpty()) {
            if (ok) {
                if (const auto *old = selected(); old && !m_renameArchived) {
                    m_organization.renameSession(old->identity, old->machine + '\n' + m_renameKey.section('\n', 1, 1));
                    saveOrganization();
                }
                m_composer->renameDraft(m_selectedKey, m_renameKey);
                if (m_localMessages.contains(m_selectedKey)) m_localMessages.insert(m_renameKey, m_localMessages.take(m_selectedKey));
                if (!m_renameArchived) m_terminal->updateSessionName(m_renameKey.section('\n', 1, 1));
                m_filter = m_renameArchived ? "archived" : "all";
                m_search->clear(); m_pages->setCurrentIndex(0); m_projectsNav->setChecked(false);
                m_details = {}; m_events = {}; m_cursor = 0; m_inspectError.clear();
                m_childrenHtml.clear();
            } else m_renameKey.clear();
        }
        if (ok && !m_forkKey.isEmpty()) {
            m_filter = "all"; m_folderFilterPath.clear(); m_hostFilters.clear(); m_projectFilters.clear(); m_search->clear(); m_pages->setCurrentIndex(0); m_projectsNav->setChecked(false);
        }
        if (!ok && !m_forkKey.isEmpty()) { m_forkKey.clear(); m_forkGroup.clear(); m_forkBefore.clear(); }
        if (!m_restoreKey.isEmpty()) {
            if (ok) { m_filter = "all"; m_folderFilterPath.clear(); m_hostFilters.clear(); m_projectFilters.clear(); m_search->clear(); m_pages->setCurrentIndex(0); m_projectsNav->setChecked(false); }
            else m_restoreKey.clear();
        }
        showNotice(ok ? (renamed ? tr("Session renamed. Refreshing…") : tr("Session updated. Refreshing…")) : detail, !ok);
        emit refreshRequested(); rebuild(); inspect();
    });
    connect(&m_client, &HgsClient::worktreeCleanupFinished, this, [this](const QString &host, const QString &operation, const QJsonObject &result) {
        if (m_archivedWorktree.size() != 4 || m_archivedWorktree[0] != host || (!m_archivedWorktree[2].isEmpty() && m_archivedWorktree[2] != result["path"].toString())
            || (!m_archivedWorktree[3].isEmpty() && m_archivedWorktree[3] != result["common_dir"].toString())
            || (operation != "archive " + m_archivedWorktree[1] && operation != "terminate " + m_archivedWorktree[1])) return;
        m_archivedWorktree[2] = result["path"].toString(); m_archivedWorktree[3] = result["common_dir"].toString();
        if (result["status"] == "removed") {
            m_worktreeCleanup->hide(); m_archivedWorktree.clear();
            showNotice(tr("Worktree removed automatically. The branch and archived conversation are kept.")); updateWorktrees();
        } else {
            m_worktreeCleanup->setText(result["status"] == "unconfirmed" ? tr("Check worktree cleanup…") : tr("Review remaining worktree…"));
            m_worktreeCleanup->setToolTip(result["reason"].toString()); m_worktreeCleanup->show();
            showNotice(tr("Worktree kept for review: %1").arg(result["reason"].toString()));
        }
    });
    connect(&m_client, &HgsClient::gitStatusChanged, this, [this](const QString &host, const QString &path) {
        // Update only the Git role. Asynchronous repository reads must not run
        // session selection/inspection or move composer focus.
        for (int i = 0; i < m_sessions->count(); ++i) {
            auto *item = m_sessions->item(i);
            if (item->data(SessionRoles::GitPath).toString() != path || item->data(SessionRoles::GitHost).toString() != host) continue;
            for (const auto &entry : m_entries) if (entry.key == item->data(KeyRole).toString()) {
                const auto data = SessionPresentation::gitStatus(entry.session, m_client.gitStatusSnapshot(host, path), entry.online);
                item->setData(SessionRoles::GitStatus, data);
                item->setData(Qt::AccessibleDescriptionRole, GitStatusBadge::tooltip(data));
                break;
            }
        }
        updateDashboard();
    });
    m_timer.setInterval(500); connect(&m_timer, &QTimer::timeout, this, [this]() {
        const bool background=m_detailTabs->currentWidget()==m_terminal || m_detailTabs->currentWidget()==m_nativeUi;
        const int interval=background?ProcessSettings::backgroundMs():m_detailTabs->currentWidget()==m_processes?ProcessSettings::activeMs():m_activityPollMs;
        if(!m_inspectionAge.isValid() || m_inspectionAge.elapsed()>=interval)inspect();
        if (++m_tick % 10 == 0) {
            emit refreshRequested();
            for (const auto &entry : m_entries) {
                const auto path = SessionPresentation::gitStatusPath(entry.session);
                if (entry.online && !path.isEmpty()) m_client.requestGitStatus(entry.host, path);
            }
            // Expire verification visually even if fleet polling has stalled.
            m_sessions->viewport()->update(); updateDashboard();
        }
    });
    m_readTimer.setInterval(400); connect(&m_readTimer, &QTimer::timeout, this, &SessionsWindow::checkViewedReply);
    applyTheme(); applyContentScale(); rebuild();
    setupShortcuts();
}

// Only session content follows this preference. Workspace chrome, including
// both sidebars, the session header and its tabs, keeps the native size.
void SessionsWindow::applyContentScale()
{
    const double scale = ContentScale::factor();
    for (auto *view : {m_activityView, m_subagentView}) view->setContentScale(scale);
    for (auto *composer : {m_composer, m_subagentComposer}) composer->setContentScale(scale);
    m_question->setContentScale(scale); m_terminal->setContentScale(scale);
    applyActivityWidth();
}

void SessionsWindow::applyActivityWidth()
{
    const int column = ActivityWidth::column(ContentScale::factor());
    for (auto *view : {m_activityView, m_subagentView}) view->setColumnWidth(column);
    for (auto *layout : m_activityColumns) layout->setColumnWidth(column);
}

void SessionsWindow::updateWindowPinAppearance()
{
    if (!m_windowPin) return;
    const bool pinned = m_windowPin->isChecked();
    const QString glyph = pinned ? QStringLiteral("pinned") : QStringLiteral("unpinned");
    const QColor color = pinned ? QColor(m_dark ? "#ffda76" : "#a66000")
                                : (m_muted.isEmpty() ? palette().color(QPalette::WindowText) : QColor(m_muted));
    // Native state polling should not rebuild the icon when nothing changed.
    if (m_windowPin->property("glyph").toString() != glyph || m_windowPin->property("iconColor").value<QColor>() != color) {
        m_windowPin->setProperty("glyph", glyph); m_windowPin->setProperty("iconColor", color);
        m_windowPin->setIcon(workspaceIcon(glyph, color));
    }
    const QString action = pinned ? tr("Unpin Zerus") : tr("Keep Zerus above other windows");
    m_windowPin->setToolTip(action); m_windowPin->setAccessibleName(action);
    m_windowPin->setAccessibleDescription(pinned ? tr("Zerus stays above other windows.") : tr("Other windows can cover Zerus."));
}

void SessionsWindow::applyTheme()
{
    const QString theme = QSettings().value("workspace/theme", "system").toString();
    m_dark = theme == "dark" || (theme != "light" && qApp->palette().color(QPalette::Window).lightness() < 128);
    m_sessionLoading->setTheme(m_dark);
    m_fg = m_dark ? "#e8edf4" : "#1a2733";
    m_muted = m_dark ? "#a2adbc" : "#627082";
    m_surface = m_dark ? "#1c2229" : "#ffffff";
    m_border = m_dark ? "#333c47" : "#dce2e8";
    m_accent = m_dark ? "#8bdfc0" : "#167357";
    const QString bg = m_dark ? "#161b21" : "#f5f7f9";
    const QString hover = m_dark ? "#2b3540" : "#edf2f5";
    const QString selected = m_dark ? "#233d35" : "#e0f0e9";
    setStyleSheet(QString(R"(
        QWidget#settingsPage, QWidget#recoverySettingsPage, QWidget#sessionsWindow, QWidget#machinesPage, QWidget#accountsPage, QDialog#accountDialog, QDialog#accountPermissionsDialog, QDialog#machineSetupDialog, QDialog#projectsPage, QDialog#projectColorDialog, QDialog#projectFolderDialog, QDialog#deleteProjectDialog, QDialog#newSessionDialog, QDialog#folderBrowser, QDialog#forkSessionDialog, QDialog#renameSessionDialog, QDialog#workspaceSettingsDialog, QDialog#sessionFileDialog, QDialog#swarmDialog, QDialog#speechProfileEditor { background:%1; color:%2; }
        QColorDialog#workspaceColorDialog { background:%1; color:%2; }
        QDialog#savedDraftsDialog { background:%1; color:%2; }
        QWidget#automaticApprovalSettings { background:transparent; color:%2; }
        QPlainTextEdit#approvalJournalDetail { background:%3; color:%2; border:1px solid %4; border-radius:8px; padding:10px; selection-background-color:%10; selection-color:%2; }
        QTableWidget#approvalJournal::item { padding:7px 10px; border-bottom:1px solid %4; }
        QListWidget#savedDraftList { background:%1; color:%2; border:1px solid %4; border-radius:8px; padding:4px; outline:0; }
        QPlainTextEdit#savedDraftPreview { background:%3; color:%2; border:1px solid %4; border-radius:8px; padding:10px; selection-background-color:%10; selection-color:%2; }
        QWidget { font-size:13px; }
        QLabel { color:%2; background:transparent; }
        QWidget#sidebar { background:%3; border-right:1px solid %4; }
        QLabel#brand { font-size:32px; font-weight:700; letter-spacing:-2px; color:%5; }
        QPushButton#appVersion { font-size:9px; color:%6; background:transparent; border:1px solid transparent; border-radius:5px; padding:0; min-height:22px; }
        QPushButton#appVersion:hover { color:%2; background:%7; }
        QPushButton#appVersion:focus[keyboardFocus="true"] { border-color:%5; }
        QLabel#eyebrow { font-size:10px; font-weight:600; letter-spacing:1px; color:%6; }
        QLabel#heading { font-size:16px; font-weight:600; }
        QLabel#detailTitle { font-size:16px; font-weight:600; }
        QLabel#detailsIdentityTitle { font-size:14px; font-weight:600; }
        QLabel#emptyTitle { font-size:21px; font-weight:600; margin-bottom:8px; }
        QLabel#muted, QLabel#footnote, QLabel#sessionMeta, QLabel#listSummary { color:%6; }
        QLabel#sessionMeta, QLabel#listSummary { font-size:11px; }
        QLabel#footnote { font-size:11px; margin-top:12px; }
        QPushButton { background:%3; color:%2; border:1px solid %4; border-radius:7px; padding:8px 13px; min-height:18px; }
        QPushButton:hover { background:%7; }
        QToolButton#manageLaunchProject { color:%6; background:transparent; border:1px solid transparent; border-radius:5px; padding:6px; }
        QToolButton#manageLaunchProject:hover { color:%2; background:%7; }
        QToolButton#manageLaunchProject:focus { border-color:%5; }
        QToolButton#manageLaunchProject:disabled { color:%6; }
        QPushButton:focus[keyboardFocus="true"] { border:1px solid %5; }
        QPushButton:disabled { color:%6; background:%1; border-color:%4; }
        QPushButton#primary, QPushButton[archiveRestore="true"] { background:%5; color:%8; border-color:%5; font-weight:600; }
        QPushButton#primary:hover, QPushButton[archiveRestore="true"]:hover { background:%9; }
        QPushButton#primary:disabled, QPushButton[archiveRestore="true"]:disabled { color:%6; background:%1; border-color:%4; }
        QPushButton#nav { text-align:left; border:1px solid transparent; padding:10px 9px; background:transparent; }
        QPushButton#nav:hover { background:%7; }
        QPushButton#nav:checked { background:%10; color:%5; font-weight:600; }
        QPushButton#nav:focus[keyboardFocus="true"] { border-color:%5; }
        QPushButton#brandMark { min-height:42px; max-height:42px; padding:0; border:1px solid transparent; background:transparent; }
        QPushButton#brandMark:hover, QPushButton#brandMark:checked { background:%10; }
        QPushButton#railButton, QPushButton#workspaceSettings, QPushButton#windowPin { min-height:42px; max-height:42px; background:transparent; border:1px solid transparent; padding:0; }
        QPushButton#railButton:hover, QPushButton#workspaceSettings:hover, QPushButton#windowPin:hover { background:%7; }
        QPushButton#workspaceSettings:checked { background:%7; border-color:%4; }
        QPushButton#windowPin:focus[keyboardFocus="true"] { border-color:%5; }
        QFrame#settingsCard { background:%3;border:1px solid %4;border-radius:9px; }
        QListWidget#settingsSections { background:transparent;color:%2;border:0;outline:0; }
        QListWidget#settingsSections::item { padding:11px 10px;margin-bottom:5px;border-radius:6px; }
        QListWidget#settingsSections::item:selected { background:%10;color:%5; }
        QListWidget#settingsSections::item:hover { background:%7; }
        QWidget#settingsPage QScrollArea, QWidget#settingsContent, QWidget#relaySettings, QWidget#speechSettings, QWidget#downloadCacheSettings, QDialog#speechProfileEditor QScrollArea, QWidget#speechEditorContent, QDialog#swarmDialog QScrollArea, QWidget#swarmConflictContent { background:transparent;border:0; }
        QDialog#speechProfileEditor QScrollBar:vertical { background:%1; width:10px; margin:0; border:0; }
        QDialog#speechProfileEditor QScrollBar:horizontal { background:%1; height:10px; margin:0; border:0; }
        QDialog#speechProfileEditor QScrollBar::handle:vertical { background:%4; border-radius:4px; min-height:24px; }
        QDialog#speechProfileEditor QScrollBar::handle:horizontal { background:%4; border-radius:4px; min-width:24px; }
        QDialog#speechProfileEditor QScrollBar::add-line, QDialog#speechProfileEditor QScrollBar::sub-line { width:0; height:0; }
        QDialog#speechProfileEditor QScrollBar::add-page, QDialog#speechProfileEditor QScrollBar::sub-page { background:transparent; }
        QPushButton#railButton:checked { background:%10; border-color:%4; }
        QPushButton#sessionFilter { padding:4px 2px; min-height:22px; font-size:11px; background:transparent; border-color:transparent; }
        QPushButton#sessionFilter:checked { background:%10; border-color:%4; color:%5; }
        QPushButton#sessionFilter:hover { background:%7; }
        QPushButton#newSession, QPushButton#sessionPanelNewSession, QPushButton#batchActions, QPushButton#sessionPanelToggle, QPushButton#savedDrafts { padding:0; background:transparent; border-color:transparent; }
        QPushButton#newSession:hover, QPushButton#sessionPanelNewSession:hover, QPushButton#batchActions:hover, QPushButton#sessionPanelToggle:hover, QPushButton#sessionStripSearch:hover, QPushButton#savedDrafts:hover { background:%7; }
        QPushButton#sessionStripSearch { padding:0; background:%3; border:1px solid %4; }
        QWidget#sessionListPanel { background:%1; }
        QPushButton#newSession { background:%10; }
        QPushButton#machineFilter::menu-indicator, QPushButton#projectFilter::menu-indicator, QPushButton#batchActions::menu-indicator { image:none; width:0; }
        QPushButton#hostFilterChip { padding:3px 8px; font-size:11px; text-align:left; color:%5; background:%10; }
        QPushButton[sessionHeaderAction="true"], QPushButton[settingReset="true"] { background:transparent; border:1px solid transparent; border-radius:6px; padding:0; }
        QPushButton[sessionHeaderAction="true"]:hover, QPushButton[settingReset="true"]:hover { background:%7; }
        QPushButton[sessionHeaderAction="true"]:checked, QPushButton[sessionHeaderAction="true"]:pressed, QPushButton[settingReset="true"]:pressed { background:%10; }
        QPushButton[sessionHeaderAction="true"]:disabled { background:transparent; border-color:transparent; }
        QPushButton[sessionHeaderAction="true"]:focus[keyboardFocus="true"], QPushButton[settingReset="true"]:focus[keyboardFocus="true"] { border-color:%5; }
        QPushButton[sessionHeaderAction="true"]::menu-indicator { image:none; width:0; }
        QMenu { background:%3; color:%2; border:1px solid %4; padding:4px; }
        QMenu::item { padding:8px 32px 8px 12px; min-width:110px; border-radius:4px; }
        QMenu::item:selected { background:%10; color:%2; }
        QMenu::item:disabled { color:%6; }
        QMenu::separator { height:1px; background:%4; margin:4px 8px; }
        QMenu::right-arrow { image:url(:/hgs/chevron-right.svg); width:14px; height:14px; right:8px; }
        QLineEdit#search { background:%3; color:%2; border:1px solid %4; border-radius:7px; padding:7px 10px; selection-background-color:%10; }
        QLineEdit#search:focus { border-color:%5; }
        QLineEdit, QComboBox { background:%3; color:%2; border:1px solid %4; border-radius:7px; padding:9px 10px; selection-background-color:%10; }
        QLineEdit:focus, QComboBox:focus { border-color:%5; }
        QLineEdit:disabled, QComboBox:disabled { color:%6; background:%1; }
        QComboBox::drop-down { border:0; width:24px; }
        QComboBox::down-arrow { image:url(:/hgs/chevron-down.svg); width:14px; height:14px; }
        QComboBox QAbstractItemView { background:%3; color:%2; selection-background-color:%10; selection-color:%2; }
        QTableWidget, QTreeWidget#worktreeCatalog, QTreeWidget#processList, QListWidget#folderList { background:%3; color:%2; border:1px solid %4; border-radius:8px; gridline-color:%4; selection-background-color:%10; selection-color:%2; }
        QHeaderView::section { background:%3; color:%6; border:0; border-bottom:1px solid %4; padding:10px; }
        QListWidget#folderList::item { padding:9px 12px; }
        QListWidget#logicalProjects, QListWidget#swarmConflicts { background:%3; color:%2; border:1px solid %4; border-radius:8px; padding:5px; outline:0; }
        QListWidget#logicalProjects::item, QListWidget#swarmConflicts::item { padding:12px 10px; border-radius:6px; margin:2px 0; }
        QListWidget#logicalProjects::item:selected, QListWidget#swarmConflicts::item:selected { background:%7; color:%2; }
        QListWidget#logicalProjects::item:hover, QListWidget#swarmConflicts::item:hover { background:%10; }
        QSplitter#swarmConflictSplit::handle { width:14px; }
        QTableWidget#swarmConflictChoices::item { padding:8px 10px; }
        QTreeWidget#worktreeCatalog::item, QTreeWidget#processList::item { padding:6px 4px; }
        QTableWidget#projectFolders::item { padding:0 10px; }
        QSpinBox, QDoubleSpinBox#downloadCacheSize { background:%3; color:%2; border:1px solid %4; border-radius:7px; padding:8px 10px; }
        QDoubleSpinBox#downloadCacheSize:disabled { color:%6; background:%1; }
        QListWidget#machineProfiles, QListWidget#accountProfiles, QListWidget#speechProfiles, QListWidget#launchAccount { background:%3; color:%2; border:1px solid %4; border-radius:8px; }
        QListWidget#machineProfiles::item, QListWidget#speechProfiles::item { padding:14px 10px; border-bottom:1px solid %4; }
        QListWidget#accountProfiles::item { padding:6px 10px; border-bottom:1px solid %4; }
        QListWidget#machineProfiles::item:selected, QListWidget#accountProfiles::item:selected, QListWidget#speechProfiles::item:selected, QListWidget#launchAccount::item:selected { background:%10; color:%2; }
        QListWidget#launchAccount::item:hover { background:%7; }
        QPlainTextEdit#machineSetupLog, QPlainTextEdit#processOutput { background:%3; color:%2; border:1px solid %4; border-radius:7px; }
        QPushButton#machineFilter, QPushButton#projectFilter { padding:0; border-color:transparent; background:transparent; }
        QCheckBox { color:%2; spacing:8px; }
        QSlider#workspaceContentScale, QSlider#workspaceActivityWidth { min-height:22px; }
        QSlider#workspaceContentScale::groove:horizontal, QSlider#workspaceActivityWidth::groove:horizontal { height:4px; background:%4; border-radius:2px; }
        QSlider#workspaceContentScale::sub-page:horizontal, QSlider#workspaceActivityWidth::sub-page:horizontal { background:%5; border-radius:2px; }
        QSlider#workspaceContentScale::handle:horizontal, QSlider#workspaceActivityWidth::handle:horizontal { background:%5; width:16px; margin:-6px 0; border-radius:8px; }
        QSlider#workspaceContentScale::handle:horizontal:hover, QSlider#workspaceActivityWidth::handle:horizontal:hover { background:%9; }
        QSlider#workspaceActivityWidth::sub-page:horizontal:disabled, QSlider#workspaceActivityWidth::handle:horizontal:disabled { background:%6; }
        QListWidget#sessionList { background:transparent; border:0; outline:0; padding-right:2px; }
        QListWidget#hosts { background:transparent; border:0; outline:0; color:%6; }
        QListWidget#hosts::item { padding:9px 8px; border-radius:6px; }
        QListWidget#hosts::item:selected { background:%7; color:%2; }
        QListWidget#hosts::item:hover { background:%7; }
        QStackedWidget#detail { background:%3; border:1px solid %4; border-radius:12px; }
        QTabWidget::pane { border:0; border-top:1px solid %4; }
        QTabBar::tab { background:transparent; color:%6; padding:8px 9px; border-bottom:2px solid transparent; }
        QTabBar::tab:selected { color:%2; border-bottom:2px solid %5; }
        QTabBar::tab:focus { color:%5; }
        QTextBrowser { background:transparent; color:%2; border:0; padding-top:12px; selection-background-color:%10; }
        QLabel#sessionModel { color:%6; font-size:11px; }
        QLabel#hint { color:%6; font-size:12px; }
        QFrame#workspaceStatus { background:%1; border:0; border-top:1px solid %4; }
        QLabel#notice { color:%6; font-size:11px; }
        QLabel#workspaceConnectionStatus { color:%12; font-size:11px; }
        QPushButton#connectionRetry { color:%2; background:transparent; border:0; padding:0 6px; min-height:18px; font-size:11px; }
        QPushButton#dismissNotice { background:transparent; border:0; padding:0; min-height:0; }
        QPushButton#dismissNotice:hover { background:%7; border-radius:4px; }
        QPushButton#updatesAvailable, QPushButton#restartToUpdate { color:%5; background:%10; border:1px solid %5; border-radius:5px; padding:0 7px; min-height:18px; max-height:18px; font-size:11px; }
        QPushButton#updatesAvailable:hover, QPushButton#restartToUpdate:hover { background:%7; }
        QLabel#notice[error="true"] { color:%12; }
        QSplitter::handle { background:transparent; width:8px; }
        QFrame#sessionInspectorPanel { background:%11; border:1px solid %4; border-radius:8px; }
        QTabWidget#sessionInspector::pane { border:0; border-top:1px solid %4; }
        QTabWidget#sessionInspector QTabBar::tab { font-size:12px; padding:8px 7px; }
        QTabWidget#sessionInspector QTextBrowser { padding:10px 2px 0; }
    )").arg(bg, m_fg, m_surface, m_border, m_accent, m_muted, hover,
              m_dark ? "#12372b" : "#ffffff", m_dark ? "#aae9d2" : "#115d46", selected,
              m_dark ? "#151b21" : "#f3f6f8", tone("Error", m_dark).name()) + workspaceScrollbars(m_dark));
    for (auto *button : findChildren<QPushButton *>()) if (button != m_windowPin && !button->property("glyph").toString().isEmpty())
        button->setIcon(workspaceIcon(button->property("glyph").toString(), QColor(button->objectName() == "updatesAvailable" || button->objectName() == "restartToUpdate" ? m_accent : m_muted)));
    updateWindowPinAppearance();
    if (m_sessionDock) m_sessionDock->setEdgeColor(QColor(m_border));
    if (m_brand) m_brand->setAppearance(m_dark ? QColor(Qt::white) : QColor(m_accent), QColor(m_dark ? "#ffda76" : "#a66000"),
        m_attentionCount > 0, QSettings().value("workspace/reduceMotion", false).toBool());
    m_sessions->setProperty("hgsDark", m_dark);
    m_sessions->setActivityAnimationEnabled(QSettings().value("workspace/animateActivity", true).toBool());
    m_projectsPage->setTheme(m_dark); m_accountsPage->setTheme(m_dark); m_machinesPage->setTheme(m_dark); m_providerBadge->setTheme(m_dark); m_machineBadge->setTheme(m_dark);
    m_accountUsage->setTheme(m_dark);
    m_usageWarning->setStyleSheet(QString("QLabel{color:%1;}").arg(m_dark?"#ffabab":"#a42330"));
    m_usageRefresh->setTheme(m_dark);
    m_contextUsage->setTheme(m_dark);m_subagentContextUsage->setTheme(m_dark);
    m_cacheStatus->setTheme(m_dark);
    m_cacheWarning->setStyleSheet(QString("QLabel{color:%1;font-size:12px;}").arg(m_dark?"#efbd78":"#91621a"));
    m_cacheDetail->setStyleSheet(QString("QLabel{color:%1;font-size:11px;}").arg(m_muted));
    m_cacheClear->setIcon(workspaceIcon("refresh",QColor(m_muted)));m_cacheClear->setIconSize(QSize(14,14));
    m_fileDrop->setTheme(m_dark);
    m_recovery->setStyleSheet(QStringLiteral("QFrame#recoveryPanel {background:transparent;border:0;} QLabel {border:0;background:transparent;}"));
    m_archiveBrowser->setTheme(m_dark);
    m_machineFilter->setTheme(m_dark); m_projectFilter->setTheme(m_dark); m_dashboard->setTheme(m_dark); m_searchResults->setTheme(m_dark); m_activityView->setTheme(m_dark); m_composer->setTheme(m_dark); m_question->setTheme(m_dark); m_terminal->setTheme(m_dark);
    m_subagentView->setTheme(m_dark); m_subagentComposer->setTheme(m_dark);
    m_processes->setTheme(m_dark);
    m_childrenHtml.clear(); m_sessions->viewport()->update(); renderDetails();
}

// The exact text sent from Activity when it is this turn's prompt, otherwise
// the recorded prompt. Clipped prompts and automatic continuations are skipped.
QString SessionsWindow::interruptedPrompt(const QString &key) const
{
    const QString prompt = m_details.value("prompt").toString();
    if (prompt.isEmpty() || prompt.startsWith(QStringLiteral("[HGS automatic recovery]"))) return {};
    const bool clipped = prompt.size() > 4000 && prompt.endsWith(QChar(0x2026));
    const QString known = clipped ? prompt.chopped(1) : prompt;
    const auto messages = m_localMessages.value(key);
    for (int i = messages.size(); i-- > 0;) {
        const auto message = messages[i].toObject(); const QString text = message.value("text").toString();
        if (message.value("status") != "error" && message.value("run_id") == m_details.value("run_id")
            && (clipped ? text.startsWith(known) : text.trimmed() == known.trimmed())) return text;
    }
    return clipped ? QString() : prompt;
}

void SessionsWindow::restoreInterruptedPrompt()
{
    const auto pending = m_interruptPrompts.constFind(m_selectedKey);
    // Wait for this session's inspection; selection briefly renders without it.
    if (pending == m_interruptPrompts.cend() || m_details.value("run_id").toString().isEmpty()) return;
    const QString phase = m_details.value("phase").toString();
    if (m_details.value("run_id") != pending->run || m_details.value("conversation_id") != pending->conversation
        || !QStringList{"working", "tool", "compacting", "interrupted"}.contains(phase)) {
        m_interruptPrompts.remove(m_selectedKey); return; // The turn ended another way.
    }
    if (phase != "interrupted") return;
    const QString text = pending->text; m_interruptPrompts.remove(m_selectedKey);
    if (m_composer->offerDraft(m_selectedKey, text)) m_composer->editor()->setFocus(Qt::OtherFocusReason);
}

void SessionsWindow::updateSessionsToggle()
{
    const auto mode = m_sessionDock->mode();
    const QString glyph = mode == SessionPanelDock::Docked ? "collapse-sessions" : mode == SessionPanelDock::Collapsed ? "expand-sessions" : "pin";
    const QString caption = mode == SessionPanelDock::Docked ? tr("Collapse session list")
        : mode == SessionPanelDock::Collapsed ? tr("Expand session list") : tr("Keep session list open");
    m_sessionsToggle->setProperty("glyph", glyph); m_sessionsToggle->setToolTip(caption); m_sessionsToggle->setAccessibleName(caption);
    m_sessionsToggle->setIcon(workspaceIcon(glyph, QColor(m_muted)));
}

// The strip keeps the list and swaps the panel chrome for compact controls:
// one toggle, a search button, the active filter and the session count.
void SessionsWindow::applySessionStrip()
{
    if (!m_sessionDock) return;
    const bool strip = m_sessionDock->chromeNarrow(), filters = m_sessionDock->filtersNarrow();
    // Rebuilds follow every poll; the layout only changes with the strip state.
    if (m_stripLayout != int(strip) * 2 + int(filters)) {
        if (strip && !(m_stripLayout & 2)) {
            // Measured before hiding, while these rows still have their docked height.
            int reserve = 0, rows = 0;
            for (QWidget *row : {static_cast<QWidget *>(m_machineFilter), static_cast<QWidget *>(m_projectFilter), static_cast<QWidget *>(m_folderFilterClear)})
                if (row->isVisible()) { reserve += row->height(); ++rows; }
            m_stripReserve = rows ? reserve + (rows - 1) * m_stripSpacer->parentWidget()->layout()->spacing() : 0;
        }
        m_stripLayout = int(strip) * 2 + int(filters);
        m_sessionHeader->setAlignment(m_sessionsToggle, strip ? Qt::AlignHCenter : Qt::Alignment());
        m_filterRow->setContentsMargins(filters ? SessionStrip::tileColumn() : QMargins());
    }
    m_heading->setVisible(!strip); m_panelNewSession->setVisible(!strip); m_batchButton->setVisible(!strip); m_savedDrafts->setVisible(!strip);
    // The search button stands in for the field at the same height and as wide
    // as a tile, border included. A widget style survives the window style's
    // repolish, which resets button minimums.
    const int side = SessionStrip::cardSide(m_sessions->property("compact").toBool());
    setWorkspaceStyle(m_stripSearch, QString("QPushButton { min-height:%1px; max-height:%1px; min-width:%2px; max-width:%2px; }")
        .arg(qMax(0, m_search->sizeHint().height() - 2)).arg(side - 2));
    // Rows hide before others show: showing a widget lays the panel out at once,
    // and a row still to go would hold the list a row lower until the next layout.
    const bool folder = !strip && !m_folderFilterPath.isEmpty(), spacer = strip && m_stripReserve > 0;
    (strip ? static_cast<QWidget *>(m_search) : m_stripSearch)->hide();
    if (strip) { m_machineFilter->setConcealed(true); m_projectFilter->setConcealed(true); }
    if (!folder) m_folderFilterClear->hide();
    if (!spacer) m_stripSpacer->hide();
    m_stripSpacer->setFixedHeight(m_stripReserve);
    (strip ? static_cast<QWidget *>(m_stripSearch) : m_search)->show();
    if (spacer) m_stripSpacer->show();
    if (!strip) { m_machineFilter->setConcealed(false); m_projectFilter->setConcealed(false); }
    if (folder) m_folderFilterClear->show();
    // Narrow filters keep the active one and the most urgent beside it: sessions
    // that need attention, otherwise active ones, otherwise all of them.
    QString urgent;
    for (const char *id : {"attention", "active", "all"}) {
        const auto found = std::find_if(m_filters.cbegin(), m_filters.cend(), [id](auto *button) { return button->property("filter").toString() == id; });
        if (found == m_filters.cend() || (*found)->isChecked() || (QString(id) != "all" && (*found)->property("count").toInt() == 0)) continue;
        urgent = id; break;
    }
    QList<QPushButton *> shown;
    for (auto *button : m_filters) {
        const bool visible = !filters || button->isChecked() || button->property("filter").toString() == urgent;
        button->setVisible(visible); if (visible) shown.append(button);
    }
    // Together they span a tile and meet in the middle.
    for (auto *button : m_filters) {
        const int index = shown.indexOf(button);
        const Qt::Alignment align = !filters ? Qt::Alignment() : shown.size() < 2 ? Qt::AlignHCenter : index == 0 ? Qt::AlignRight : Qt::AlignLeft;
        const int half = (side - m_filterRow->spacing()) / 2;
        if (filters) button->setFixedWidth(shown.size() < 2 ? side : index == 0 ? half : side - m_filterRow->spacing() - half);
        else { button->setMinimumWidth(0); button->setMaximumWidth(QWIDGETSIZE_MAX); }
        if (m_filterRow->itemAt(m_filterRow->indexOf(button))->alignment() != align) m_filterRow->setAlignment(button, align);
    }
    m_machineFilter->button()->setVisible(!filters); m_projectFilter->button()->setVisible(!filters);
    m_listStack->setCurrentIndex(!strip && m_searchResults->active() ? 1 : 0);
    m_count->setText(strip ? m_countShort : m_countFull);
    m_count->setAlignment(strip ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
    m_count->setVisible(strip || !m_searchResults->active());
    if (!strip && m_focusSearch) { m_focusSearch = false; m_search->setFocus(Qt::OtherFocusReason); }
}

void SessionsWindow::updateInspectorMinimum()
{
    if(!m_inspectorPanel || !m_inspectorToggle || !m_splitter)return;
    const int header=qMax(240,m_inspector->tabBar()->sizeHint().width()+m_inspectorToggle->sizeHint().width()+32);
    if(m_inspectorPanel->minimumWidth()!=header)m_inspectorPanel->setMinimumWidth(header);
    int minimum=840;
    if(!m_inspectorPanel->isHidden() && m_pages && m_pages->currentIndex()==0) {
        const auto body=m_splitter->parentWidget()->layout()->contentsMargins();
        const auto detail=m_workSplitter->parentWidget()->layout()->contentsMargins();
        const auto frame=m_detailStack->contentsMargins();
        const int rail=layout()->itemAt(0)->widget()->width();
        // An explicit window minimum overrides Qt's automatic layout minimum.
        // Account for the full header through both nested splitters and margins.
        minimum=qMax(minimum,rail+body.left()+body.right()+detail.left()+detail.right()+frame.left()+frame.right()
            +m_splitter->handleWidth()+m_workSplitter->handleWidth()+m_splitter->widget(0)->minimumWidth()
            +m_detailTabs->minimumWidth()+header);
    }
    setMinimumWidth(minimum);
}

void SessionsWindow::setInspectorVisible(bool visible)
{
    if (!visible && !m_inspectorPanel->isHidden()) m_inspectorWidth = m_inspectorPanel->width();
    // One control follows the right edge, rather than offering two hide actions.
    const bool restoreFocus = m_inspectorToggle->hasFocus();
    const bool keyboardFocus = m_inspectorToggle->property("keyboardFocus").toBool();
    auto *oldHeader = visible ? m_detailTabs : m_inspector;
    auto *newHeader = visible ? m_inspector : m_detailTabs;
    if (newHeader->cornerWidget() != m_inspectorToggle) {
        oldHeader->setCornerWidget(nullptr);
        newHeader->setCornerWidget(m_inspectorToggle);
    }
    m_inspectorPanel->setVisible(visible);
    const QSignalBlocker blocker(m_inspectorToggle); m_inspectorToggle->setChecked(visible);
    m_inspectorToggle->setProperty("glyph", visible ? "collapse-panel" : "inspector");
    m_inspectorToggle->setIcon(workspaceIcon(m_inspectorToggle->property("glyph").toString(), QColor(m_muted)));
    m_inspectorToggle->setAccessibleName(visible ? tr("Collapse side panel") : tr("Show tasks, agents, details and worktrees"));
    m_inspectorToggle->setToolTip(ShortcutRegistry::instance().tooltip(m_inspectorToggle->accessibleName(), "panel.toggle"));
    m_inspectorToggle->show();
    if (restoreFocus) m_inspectorToggle->setFocus(keyboardFocus ? Qt::TabFocusReason : Qt::MouseFocusReason);
    if (visible) {
        // QTabWidget's own minimum width can ignore its full header in document
        // mode. Give the splitter an explicit minimum including the corner button.
        m_inspectorPanel->setMinimumWidth(qMax(240,m_inspector->tabBar()->sizeHint().width()+m_inspectorToggle->sizeHint().width()+32));
        const int available = m_workSplitter->width() - m_workSplitter->handleWidth();
        const int side = qBound(240, m_inspectorWidth, qMax(240, available - 280));
        m_workSplitter->setSizes({qMax(280, available - side), side});
    }
    updateInspectorMinimum();
    QSettings settings; settings.setValue("workspace/inspectorVisible", visible);
    settings.setValue("workspace/inspectorWidth", m_inspectorWidth);
    updateWorktrees();
}

void SessionsWindow::saveOrganization()
{
    if(m_swarm)m_swarm->edit(m_organization.toJson());
    const auto data = QJsonDocument(m_organization.toJson()).toJson(QJsonDocument::Compact);
    if (data == m_savedOrganization) return;
    QSettings settings; settings.setValue("workspace/organization", data); settings.sync();
    if (settings.status() == QSettings::NoError) m_savedOrganization = data;
    else showNotice(tr("Could not save the session layout. Check the settings file permissions."), true);
}

void SessionsWindow::updateHeaderText()
{
    // Always reserve the indicator's space; appearing must not re-elide the title.
    m_title->setText(m_title->fontMetrics().elidedText(m_fullTitle, Qt::ElideMiddle, qMax(20, m_title->width() - 22)));
    m_sessionLoading->move(qMin(m_title->fontMetrics().horizontalAdvance(m_title->text()) + 6, m_title->width() - 16),
        qMax(0, (m_title->height() - 16) / 2));
    m_meta->setText(m_meta->fontMetrics().elidedText(m_fullMeta, Qt::ElideMiddle, qMax(20, m_meta->width())));
    m_title->setAccessibleName(m_fullTitle); m_meta->setAccessibleName(m_fullMeta);
}

void SessionsWindow::updateSessionLoading()
{
    if (!m_sessionLoading) return;
    const auto *entry = selected();
    bool pending = false; int remaining = 2000;
    const auto consider = [&](const QElapsedTimer &age) {
        pending = true; remaining = qMin(remaining, int(qMax<qint64>(0, 2000 - age.elapsed())));
    };
    if (entry && entry->online && !entry->host.isEmpty()) {
        if (m_remotePollAges.contains(entry->host)) consider(m_remotePollAges.value(entry->host));
        for (const auto &read : m_sessionReads)
            if (read.host == entry->host && read.name == entry->session.name && read.archive == entry->session.archiveId
                && (read.agent.isEmpty() || read.agent == m_subagentId)) consider(read.age);
    }
    m_sessionLoading->setRunning(pending && remaining == 0);
    m_title->setAccessibleDescription(pending && remaining == 0 ? tr("Loading data from the remote machine") : QString());
    if (pending && remaining > 0) m_sessionLoadingTimer.start(remaining);
    else m_sessionLoadingTimer.stop();
}

void SessionsWindow::updateSessionFeedback()
{
    if (!m_sessionFeedback) return;
    const bool outsideComposer = m_detailTabs->currentWidget() != m_activityStack || !m_subagentId.isEmpty()
        || !m_composer->isInputVisible();
    const bool visible = m_pages->currentIndex() == 0 && selected() && outsideComposer
        && m_hint->property("active").toBool() && m_notice->isHidden();
    m_sessionFeedback->setText(visible ? m_hint->text() : QString());
    m_sessionFeedback->setToolTip(Qt::convertFromPlainText(m_sessionFeedback->text()));
    m_sessionFeedback->setAccessibleName(m_sessionFeedback->text());
    setWorkspaceStyle(m_sessionFeedback, QString("color:%1;").arg(m_hint->property("error").toBool()
        ? (m_dark ? "#f4ab9b" : "#a13224") : (m_dark ? "#efc978" : "#92641b")));
    m_sessionFeedback->setVisible(visible);
}

void SessionsWindow::revealSession(const QString &key)
{
    for (const auto &entry : m_entries) if (entry.key == key) {
        const auto id = m_organization.groupFor(entry.identity);
        if (const auto *group = m_organization.group(id); group && group->collapsed) {
            m_organization.setCollapsed(id, false); saveOrganization(); rebuild();
        }
        return;
    }
}

void SessionsWindow::createGroup(const QString &session)
{
    bool ok = false;
    const auto name = QInputDialog::getText(this, tr("New project"), tr("Project name"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    const auto id = m_organization.createGroup(name);
    if (!session.isEmpty()) m_organization.moveSession(session, id);
    saveOrganization(); rebuild();
}

void SessionsWindow::showGroupMenu(const QString &id, const QPoint &position)
{
    const auto *group = m_organization.group(id); if (!group) return;
    auto *menu = new QMenu(this); menu->setObjectName("sessionGroupMenu");
    connect(menu->addAction(tr("New session…")), &QAction::triggered, this,
            [this, id]() { showNewSession({}, id, selectedHostTarget()); });
    menu->addSeparator();
    auto *only = menu->addAction(workspaceIcon("projects", QColor(m_muted)), tr("Filter this project")); only->setObjectName("filterOnlyProject");
    connect(only, &QAction::triggered, this, [this,id] { if (m_organization.group(id)) m_projectFilter->setSelection({id}); });
    menu->addSeparator();
    const bool collapsed = group->collapsed;
    connect(menu->addAction(collapsed ? tr("Expand project") : tr("Collapse project")), &QAction::triggered, this, [this, id, collapsed]() {
        m_organization.setCollapsed(id, !collapsed); saveOrganization(); rebuild();
    });
    auto *rename = menu->addAction(tr("Rename project…")); rename->setEnabled(true);
    connect(rename, &QAction::triggered, this, [this, id]() {
        const auto *g = m_organization.group(id); if (!g) return;
        bool ok = false; const auto title = QInputDialog::getText(this, tr("Rename project"), tr("Project name"), QLineEdit::Normal, g->name, &ok);
        if (ok) { m_organization.renameGroup(id, title); saveOrganization(); rebuild(); }
    });
    auto *settings = menu->addAction(tr("Project settings…"));
    connect(settings, &QAction::triggered, this, [this,id] { showProjects(); m_projectsPage->selectProject(id); });
    menu->addSeparator();
    const auto groups = m_organization.groups(); int index = 0;
    while (index < groups.size() && groups[index].id != id) ++index;
    auto *up = menu->addAction(tr("Move project up")); up->setEnabled(index > 0);
    auto *down = menu->addAction(tr("Move project down")); down->setEnabled(index + 1 < groups.size());
    connect(up, &QAction::triggered, this, [this, id, groups, index]() {
        if (index > 0) { m_organization.moveGroup(id, groups[index - 1].id); saveOrganization(); rebuild(); }
    });
    connect(down, &QAction::triggered, this, [this, id, groups, index]() {
        m_organization.moveGroup(id, index + 2 < groups.size() ? groups[index + 2].id : QString()); saveOrganization(); rebuild();
    });
    auto *sort = menu->addAction(tr("Sort sessions by name"));
    connect(sort, &QAction::triggered, this, [this, id]() {
        QList<Entry> members;
        for (const auto &e : m_entries) if (m_organization.groupFor(e.identity) == id) members.append(e);
        std::stable_sort(members.begin(), members.end(), [](const Entry &a, const Entry &b) {
            const int compared = QString::localeAwareCompare(sessionLabel(a.session), sessionLabel(b.session));
            return compared ? compared < 0 : a.identity < b.identity;
        });
        for (const auto &e : members) m_organization.moveSession(e.identity, id);
        saveOrganization(); rebuild();
    });
    menu->addSeparator();
    auto *remove = menu->addAction(tr("Delete project…")); remove->setEnabled(groups.size()>1);
    connect(remove, &QAction::triggered, this, [this, id]() { m_projectsPage->deleteProject(id); });
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater); menu->popup(position);
}

QString SessionsWindow::terminalDropTarget() const
{
    const auto *entry=selected();
    if(!entry||!entry->online||m_detailTabs->currentWidget()!=m_terminal||!m_terminal->isConnected())return {};
    return m_selectedKey+'\n'+QString::number(m_terminal->connectionGeneration());
}

void SessionsWindow::pasteTerminalPaths(const QStringList &paths)
{
    // A local Windows session gets native paths, in double quotes when needed (no
    // Windows name contains one) or, with `$` or a backtick that PowerShell expands
    // there, in single quotes with `'` doubled; POSIX machines, including every peer,
    // single quotes.
    const auto *entry=selected();
    const bool windows=entry&&MachinePlatform::of(m_fleet.local().metrics,entry->host.isEmpty())==MachinePlatform::Style::Windows;
    static const QRegularExpression plain("^[\\w.:\\\\~+-]+$",QRegularExpression::UseUnicodePropertiesOption);
    QStringList quoted;
    for(auto path:paths) {
        if(path.contains(QRegularExpression("[\\x00-\\x1f\\x7f]"))) {showNotice(tr("File names containing control characters cannot be pasted into Terminal."),true);return;}
        if(windows) {
            path=MachinePlatform::native(path,MachinePlatform::Style::Windows);
            if(path.contains('$')||path.contains('`'))quoted<<'\''+path.replace('\'',QStringLiteral("''"))+'\'';
            else quoted<<(plain.match(path).hasMatch()?path:'"'+path+'"');
            continue;
        }
        path.replace('\'',QStringLiteral("'\\''"));quoted<<'\''+path+'\'';
    }
    m_terminal->pasteText(quoted.join(' ')+" ");m_terminal->setFocus(Qt::OtherFocusReason);
}

void SessionsWindow::dropTerminalFiles(const QStringList &paths)
{
    const auto *entry=selected();if(!entry||terminalDropTarget().isEmpty())return;
    if(m_terminalDropRequest){showNotice(tr("Wait for the current file transfer to finish."));return;}
    if(entry->host.isEmpty()){pasteTerminalPaths(paths);return;}
    const auto run=m_details.value("run_id").toString(entry->session.runId);
    const auto conversation=m_details.value("conversation_id").toString(entry->session.conversationId);
    // An untracked shell (hgs sh -d) never gets a run identity: its listed
    // creation time binds the copy to that live session instead.
    const bool untracked=run.isEmpty()&&conversation.isEmpty()&&!entry->session.tracked&&!m_details.value("tracked").toBool();
    if(run.isEmpty()&&!untracked){showNotice(tr("Wait for the session identity to load, then drop the files again."),true);return;}
    m_terminalDropKey=terminalDropTarget();m_terminalDropCount=paths.size();m_terminalDropUntracked=untracked?entry->machine:QString();
    m_terminalDropRequest=m_client.requestStageFiles(entry->host,entry->session.name,paths,run,conversation,untracked?entry->session.created:-1);
    showNotice(tr("Transferring files to %1…").arg(entry->machine));
}

void SessionsWindow::showUpdates(){m_pages->setCurrentIndex(5);m_settingsPage->openUpdates();}

void SessionsWindow::showWorkspaceSettings()
{
    m_pages->setCurrentIndex(5);m_settingsPage->refresh();
}

void SessionsWindow::setFleet(const FleetState &fleet) {
    // A native clean exit may remove its managed checkout without a GUI write.
    // Expire only repositories whose exact live run has newly reached Archive.
    QSet<QString> expired;
    const auto completed = [&](const BoxState &before, const BoxState &after, const QString &host) {
        for (const auto &old : before.sessions) {
            if (old.state == "archived" || !old.gitWorktree || old.gitCommonDir.isEmpty() || old.runId.isEmpty()) continue;
            for (const auto &current : after.sessions) if (current.state == "archived" && current.runId == old.runId && current.conversationId == old.conversationId) {
                const auto key=host+'\n'+old.gitCommonDir;
                if (!expired.contains(key)) { expired.insert(key); m_client.invalidateWorktreeCache(host,old.gitCommonDir); }
                break;
            }
        }
    };
    completed(m_fleet.local(),fleet.local(),{});
    for (const auto &host : fleet.peerNames()) if (const auto *before=m_fleet.peer(host)) completed(*before,*fleet.peer(host),host);
    if (auto *launch=qobject_cast<NewSessionDialog *>(QApplication::activeModalWidget()); launch && launch->parentWidget()==this) launch->setFleet(fleet);
    m_settingsPage->setLocalSessions(fleet.local().localSessions, LocalSessionHost::canEnable(fleet.local())); m_settingsPage->setPeers(fleet.peerNames());
    m_settingsPage->setMobileGateway(fleet.local().localSessions&&MachinePlatform::of(fleet.local().metrics,true)==MachinePlatform::Style::Posix);
    m_fleet = fleet; m_fleet.setAttentionMarks(QJsonDocument::fromJson(QSettings().value("attention/sessionMarks").toByteArray()).object()); m_fleet.setPendingReads(ReadMarkStore::load()); reconcileTerminations();m_usageStore->setFleet(fleet); m_accountsPage->setFleet(fleet); updateDashboard(); m_projectsPage->setFleet(fleet); m_machinesPage->setFleet(fleet); if (isVisible()) rebuild(); applyQueuedModelSettings();
    updateConnectionStatus();
    if (!m_nativeLaunchToOpen.isEmpty()) {
        for (const auto &entry : m_entries) if (entry.session.launchId==m_nativeLaunchToOpen) {
            const QString host=entry.host, name=entry.session.name;
            m_nativeLaunchToOpen.clear();showSession(host,name);break;
        }
    }
}
void SessionsWindow::setClipboardMode(bool clipboard) { m_clipboard = clipboard; renderDetails(); }
void SessionsWindow::updateDashboard()
{
    // Overview renders while shown; showing it renders the latest fleet.
    if (!isVisible() || m_pages->currentIndex() != 3) { m_dashboardStale = true; return; }
    m_dashboardStale = false;
    auto fleet = m_fleet;
    auto enrichGit = [this](BoxState box, const QString &host, bool online) {
        for (auto &session : box.sessions) {
            const auto path = SessionPresentation::gitStatusPath(session);
            session.gitStatus = SessionPresentation::gitStatus(session, m_client.gitStatusSnapshot(host, path), online);
        }
        return box;
    };
    const auto now = QDateTime::currentMSecsSinceEpoch();
    fleet.setLocal(enrichGit(fleet.local(), {}, fleet.local().ok && m_connectionError.isEmpty()), now);
    for (const auto &host : fleet.peerNames()) {
        const auto box = *fleet.peer(host);
        auto display = enrichGit(box, host, box.ok && now - fleet.peerPolledAt(host) < FleetState::kPeerStaleMs);
        // FleetState keeps existing sessions on a failed poll. Enrich those
        // retained rows in this display copy, then preserve the offline state.
        display.ok = true;
        fleet.setPeer(display, fleet.peerPolledAt(host));
        if (!box.ok) fleet.setPeer(box, fleet.peerPolledAt(host));
    }
    if (!m_connectionError.isEmpty()) { auto local = fleet.local(); local.ok = false; local.error = m_connectionError; fleet.setLocal(local, 0); }
    QSet<QString> drafts;
    const auto collectDrafts = [this, &drafts](const QString &host, const BoxState &box) {
        for (const auto &session : box.sessions) {
            const QString key = host + '\n' + session.name;
            if (entryHasDraft({host, host.isEmpty() ? box.host : host, key, {}, session, box.ok})) drafts.insert(key);
        }
    };
    collectDrafts({}, fleet.local());
    for (const auto &host : fleet.peerNames()) collectDrafts(host, *fleet.peer(host));
    m_dashboard->setFleet(fleet, drafts);
}
void SessionsWindow::setConnectionError(const QString &error) {
    if (m_connectionError == error) return;
    m_connectionError = error; updateDashboard(); updateConnectionStatus(); if (isVisible()) rebuild();
}

void SessionsWindow::setPollingHosts(const QSet<QString> &hosts)
{
    for (auto it = m_remotePollAges.begin(); it != m_remotePollAges.end();) {
        if (!hosts.contains(it.key())) it = m_remotePollAges.erase(it); else ++it;
    }
    for (const auto &host : hosts) if (!host.isEmpty() && !m_remotePollAges.contains(host)) {
        QElapsedTimer age; age.start(); m_remotePollAges.insert(host, age);
    }
    m_pollingHosts = hosts;
    updateConnectionStatus();
    updateSessionLoading();
}

void SessionsWindow::updateConnectionStatus()
{
    QStringList messages;
    bool canRetry = false;
    const auto add = [&](const QString &host, const QString &name, QString error, bool waiting) {
        const bool polling = m_pollingHosts.contains(host);
        if (waiting) {
            messages << (polling ? tr("%1: connecting…") : tr("%1: waiting for connection")).arg(name);
        } else {
            if (error == "timed out") error = host.isEmpty() ? tr("session update timed out") : tr("SSH request timed out");
            if (error.isEmpty() || error == "offline") error = tr("machine unavailable");
            if (error == "bad_response") error = tr("invalid response from hgs");
            messages << tr("%1: %2 (%3)").arg(name, error.simplified(),
                polling ? tr("retrying…") : tr("automatic retry pending"));
        }
        canRetry |= !polling;
    };
    const auto &local = m_fleet.local();
    if (!m_connectionError.isEmpty()) add({}, local.host.isEmpty() ? tr("This machine") : local.host, m_connectionError, false);
    else if (!local.ok) add({}, local.host.isEmpty() ? tr("This machine") : local.host, local.error, local.error.isEmpty());
    for (const auto &host : m_fleet.peerNames()) {
        const auto *peer = m_fleet.peer(host);
        if (!peer || !peer->ok) add(host, host, peer ? peer->error : QString(), m_fleet.peerPolledAt(host) == 0);
    }
    m_connectionStatus->setText(messages.join("; "));
    m_connectionStatus->setToolTip(Qt::convertFromPlainText(messages.join('\n')));
    m_connectionRetry->setVisible(!messages.isEmpty());
    m_connectionRetry->setEnabled(canRetry);
}

void SessionsWindow::showProjects(const QString &host)
{
    m_pages->setCurrentIndex(1); m_projectsNav->setChecked(true); m_sessionsNav->setChecked(false); m_machinesNav->setChecked(false);
    for (auto *button : m_filters) button->setChecked(false);
    m_projectsPage->selectHost(host);
}

void SessionsWindow::showSessionList()
{
    m_pages->setCurrentIndex(0); m_projectsNav->setChecked(false); m_sessionsNav->setChecked(true); rebuild();
}

void SessionsWindow::showSession(const QString &host, const QString &name)
{
    m_filter = "all"; m_folderFilterPath.clear(); m_hostFilters.clear(); m_projectFilters.clear(); m_search->clear(); showSessionList();
    const QString key = host + '\n' + name;
    revealSession(key);
    for (int row = 0; row < m_sessions->count(); ++row)
        if (m_sessions->item(row)->data(KeyRole).toString() == key) {
            m_sessions->setCurrentRow(row); m_sessions->scrollToItem(m_sessions->item(row)); return;
        }
}

void SessionsWindow::showAttentionSession(const QString &host, const QString &name)
{
    m_detailTabs->setCurrentIndex(0);
    showSession(host, name);
    inspect();
}

void SessionsWindow::populateBatchActions(QMenu *menu)
{
    // clear() only detaches child menus; delete them before populating anew.
    qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    menu->clear();
    auto addMachine = [&](const QString &host, const BoxState &box, bool online) {
        const QString machine = host.isEmpty() ? box.host : host;
        if (machine.isEmpty()) return;
        auto *group = menu->addMenu(QString(machine).replace('&', "&&"));
        int live = 0, saved = 0;
        bool allIdle = true, nativeControls = false;
        for (const auto &s : box.sessions) {
            if (s.state == "archived") continue;
            if (s.cmd=="opencode" || !s.bulkLifecycleSupported) {nativeControls=true;continue;}
            if (s.state == "running") {
                ++live; allIdle = allIdle && s.resumable && s.activity == "idle" && s.processState != "exited";
            } else if (s.resumable) ++saved;
        }
        if(nativeControls) group->addAction(tr("OpenCode uses individual Pause / Resume controls"))->setEnabled(false);
        auto *pause = group->addAction(tr("Pause all (%1)").arg(live));
        pause->setEnabled(online && !m_pending && m_renameKey.isEmpty() && live > 0 && allIdle);
        if (live && !allIdle) {
            auto *reason = group->addAction(tr("Some sessions are busy or context is not tracked"));
            reason->setEnabled(false);
        }
        if(nativeControls) pause->setToolTip(tr("OpenCode sessions are excluded; use their individual Pause / Resume controls."));
        auto *resume = group->addAction(tr("Resume all in background (%1)").arg(saved));
        if(nativeControls) resume->setToolTip(pause->toolTip());
        resume->setEnabled(online && !m_pending && m_renameKey.isEmpty() && saved > 0);
        group->setEnabled(online);
        connect(pause, &QAction::triggered, this, [this, host, machine, live]() {
            if (m_pending) return;
            m_pending = true; m_restoreKey.clear(); m_client.pauseAll(host, live);
            showNotice(tr("Pausing sessions on %1…").arg(machine)); renderDetails();
        });
        connect(resume, &QAction::triggered, this, [this, host, machine, saved]() {
            if (m_pending || !LocalSessionHost::confirmLaunch(this, m_fleet.local(), host) || m_pending) return;
            m_pending = true; m_restoreKey.clear(); m_client.resumeAll(host, saved);
            showNotice(tr("Resuming sessions on %1…").arg(machine)); renderDetails();
        });
    };
    if (m_fleet.local().localSessions) addMachine({}, m_fleet.local(), m_fleet.local().ok && m_connectionError.isEmpty());
    for (const auto &host : m_fleet.peerNames())
        if (const auto *box = m_fleet.peer(host))
            addMachine(host, *box, box->ok && QDateTime::currentMSecsSinceEpoch()
                - m_fleet.peerPolledAt(host) < FleetState::kPeerStaleMs);
    if (menu->actions().isEmpty()) menu->addAction(tr("Waiting for machines…"))->setEnabled(false);
    menu->addSeparator(); menu->addAction(m_markAllReadAction);
    menu->addAction(m_clearArchiveAction);
}

void SessionsWindow::showNewSession(const QString &agent, const QString &project, const QString &host, const QString &folder, const QString &path, const QString &account)
{
    QString group = m_organization.defaultProject();
    if (const auto *row = m_sessions->currentItem()) group = row->data(SessionRoles::Group).toString();
    else if (const auto *entry = selected()) group = m_organization.groupFor(entry->identity);
    QString launchProject = group;
    if (!project.isEmpty()) for (const auto &p : m_organization.groups()) if (p.id == project || p.name == project) { launchProject=p.id; break; }
    // Only generic creation inherits the selected session. Folder/worktree and
    // account actions already carry an explicit launch target.
    const auto *row=m_sessions->currentItem();
    const bool sessionRow=!row||(!row->data(SessionRoles::Header).toBool()&&row->data(SessionRoles::LaunchId).toString().isEmpty());
    const auto *context=sessionRow&&project.isEmpty()&&folder.isEmpty()&&path.isEmpty()&&account.isNull()?selected():nullptr;
    NewSessionDialog dialog(m_client.executable(), m_fleet, context?context->host:host, agent, launchProject, this, m_usageStore);
    dialog.setGroups(m_organization, launchProject);
    if(context)dialog.prefillSessionFolder(selectedDirectory(),m_details.value("cwd_canonical").toString(context->session.canonicalCwd));
    if(!folder.isEmpty()&&!dialog.selectFolder(folder)){showNotice(tr("This project folder changed. Select it again."),true);return;}
    if(!path.isEmpty())dialog.selectPath(path, false);
    if(!account.isNull())dialog.selectAccount(account);
    connect(&dialog, &NewSessionDialog::launchRequested, this,
        [this](const QString &host, const QString &cmd, const QString &target, const QString &name, const QString &account, const QString &group, bool openTerminal, const QString &addFolder, const QString &model) {
            const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m_pendingLaunches.insert(id, QJsonObject{{"host", host}, {"cmd", cmd}, {"tag", name}, {"group", group},
                {"target", target}, {"add_folder", !addFolder.isEmpty()}, {"folder", addFolder}, {"show_pending", true}, {"created", QDateTime::currentSecsSinceEpoch()}});
            savePendingLaunches();
            m_organization.setCollapsed(group, false);
            m_filter = "all"; m_folderFilterPath.clear(); m_hostFilters.clear(); m_projectFilters.clear(); m_search->clear(); showSessionList();
            for (int row = 0; row < m_sessions->count(); ++row)
                if (m_sessions->item(row)->data(SessionRoles::LaunchId).toString() == id) m_sessions->scrollToItem(m_sessions->item(row));
            // Refresh soon after terminal creation, while the placeholder already
            // gives feedback. Stop these extra polls once the launch is observed.
            for (int delay : {750, 2000}) QTimer::singleShot(delay, this, [this, id] {
                if (m_pendingLaunches.contains(id)) emit refreshRequested();
            });
            if (!openTerminal || cmd=="dsh" || cmd=="opencode") {
                showNotice(tr("Starting session…"));
                m_client.launchDetachedSession(host,cmd,target,name,account,id,model);
            } else emit newSessionRequested(host, cmd, target, name, account, id);
        });
    connect(&dialog, &NewSessionDialog::manageProjectsRequested, this, &SessionsWindow::showProjects);
    connect(&dialog, &NewSessionDialog::projectFoldersRequested, this, [this](const QString &id) { showProjects(); m_projectsPage->selectProject(id); });
    connect(&dialog, &NewSessionDialog::machinesRequested, this, [this] { runShortcut(QStringLiteral("go.machines")); m_machinesPage->selectMachine({}); });
    dialog.exec();
}

void SessionsWindow::savePendingLaunches()
{
    QSettings settings; settings.setValue("workspace/pendingLaunches", QJsonDocument(m_pendingLaunches).toJson(QJsonDocument::Compact)); settings.sync();
}

bool SessionsWindow::takeEndedLaunch(const QString &launchId, QJsonObject *launch)
{
    const auto it = std::find_if(m_endedLaunches.begin(), m_endedLaunches.end(), [&](const auto &ended) { return ended.first == launchId; });
    if (it == m_endedLaunches.end()) return false;
    if (launch) *launch = it->second;
    m_endedLaunches.erase(it); return true;
}

void SessionsWindow::newSessionLaunchFailed(const QString &launchId, const QString &error)
{
    // A listing that reported the launch ended already showed its reason and last
    // lines. A hidden launch status showed nothing, so its failure still names it.
    QJsonObject ended;
    if (takeEndedLaunch(launchId, &ended) && ended.value("show_pending").toBool()) return;
    const QString name = (ended.isEmpty() ? m_pendingLaunches.value(launchId).toObject() : ended).value("tag").toString();
    m_pendingLaunches.remove(launchId); savePendingLaunches();
    rebuild();
    showNotice(error.isEmpty() ? tr("Could not start session %1. Check the terminal.").arg(name)
        : tr("Could not start session %1: %2").arg(name, error), true);
}

void SessionsWindow::placeLaunchedSessions()
{
    bool changed = false;
    QString ended, endedLaunch;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = m_pendingLaunches.begin(); it != m_pendingLaunches.end();) {
        const auto launch = it.value().toObject();
        // Failed commands never produce this token. Expire abandoned intents,
        // while allowing an SSH password prompt or app restart to take its time.
        bool consumed = QUuid(it.key()).isNull() || now - launch.value("created").toInteger() > 7 * 24 * 60 * 60;
        for (const auto &entry : m_entries) {
            const auto &session = entry.session;
            if (consumed || !entry.online || session.state == "archived" || entry.host != launch.value("host").toString()
                || session.launchId != it.key() || session.cmd != launch.value("cmd").toString()
                || session.name.section('/', 2) != launch.value("tag").toString()) continue;
            const QString requested = launch.value("group").toString();
            const QString group = m_organization.group(requested) ? requested : m_organization.defaultProject();
            // A generated worktree launches in its target but adds the folder it
            // came from. Launches saved before "folder" existed add their target.
            const auto path=launch.value("add_folder").toBool()?launch.value("folder").toString(launch.value("target").toString()):QString();
            const auto style=MachinePlatform::of(m_fleet.local().metrics,entry.host.isEmpty());
            if(MachinePlatform::absolute(path,style))m_organization.addFolder(group,entry.machine,path,{},style);
            m_organization.moveSession(entry.identity, group); m_organization.setCollapsed(group, false); consumed = true;
        }
        // A session that ended before any listing showed it, leaving no record,
        // never appears: its machine reports how the launch ended instead.
        const QString host = launch.value("host").toString();
        const BoxState *box = host.isEmpty() ? &m_fleet.local() : m_fleet.peer(host);
        const auto end = !consumed && box && box->ok ? box->endedLaunches.value(it.key()) : QJsonObject();
        if (!end.isEmpty() && std::none_of(m_entries.cbegin(), m_entries.cend(), [&](const Entry &entry) {
                return entry.host == host && entry.session.launchId == it.key(); })) {
            consumed = true; m_endedLaunches.append({it.key(), launch});
            // Terminal launches and launches restored after a restart never
            // report a result, so only the newest ended launches are kept.
            constexpr qsizetype EndedLaunchLimit = 16;
            if (m_endedLaunches.size() > EndedLaunchLimit) m_endedLaunches.removeFirst();
            if (m_nativeLaunchToOpen == it.key()) m_nativeLaunchToOpen.clear();
            // A hidden launch status stays hidden.
            if (launch.value("show_pending").toBool()) {
                // Only the last ended notice shows. A launch whose notice it
                // replaces showed nothing, so its later failure still names it.
                for (auto &[id, shown] : m_endedLaunches) if (id == endedLaunch) shown["show_pending"] = false;
                endedLaunch = it.key();
                const QString name = launch.value("tag").toString(); const auto code = end.value("code");
                ended = code.isDouble() ? tr("Session %1 ended before it appeared (exit code %2).").arg(name).arg(code.toInteger())
                    : tr("Session %1 ended before it appeared.").arg(name);
                const QString output = end.value("output").toString().trimmed();
                if (!output.isEmpty()) ended += '\n' + output;
            }
        }
        if (consumed) { it = m_pendingLaunches.erase(it); changed = true; } else ++it;
    }
    if (changed) { saveOrganization(); savePendingLaunches(); }
    if (!ended.isEmpty()) showNotice(ended, true);
}

bool SessionsWindow::isTerminating(const Entry &entry) const
{
    const auto it=m_terminating.constFind(entry.key);
    return it!=m_terminating.cend() && it->session.runId==entry.session.runId && it->session.created==entry.session.created;
}

void SessionsWindow::reconcileTerminations()
{
    for(auto it=m_terminating.begin();it!=m_terminating.end();){
        const auto &entry=it.value();
        const auto *box=entry.host.isEmpty()?&m_fleet.local():m_fleet.peer(entry.host);
        const bool online=box&&box->ok&&(entry.host.isEmpty()?m_connectionError.isEmpty():
            QDateTime::currentMSecsSinceEpoch()-m_fleet.peerPolledAt(entry.host)<FleetState::kPeerStaleMs);
        if(!online||!m_terminationFinished.contains(it.key())){++it;continue;}
        const bool remains=std::any_of(box->sessions.cbegin(),box->sessions.cend(),[&](const SessionInfo &s){
            return s.state=="running"&&s.name==entry.session.name&&s.runId==entry.session.runId&&s.created==entry.session.created;
        });
        if(remains){++it;continue;}
        for(const auto &archived:box->sessions){
            if(archived.state!="archived"||archived.name!=entry.session.name||archived.runId!=entry.session.runId||archived.archiveId.isEmpty())continue;
            const auto group=m_organization.groupFor(entry.identity);
            const auto archiveIdentity=m_organization.observe(entry.machine,archived.name,archived.runId,archived.archiveId);
            m_organization.moveSession(archiveIdentity,group);saveOrganization();
        }
        m_terminationFinished.remove(it.key());it=m_terminating.erase(it);
    }
}

QString SessionsWindow::selectedHostTarget() const {
    if (m_hostFilters.size() != 1 || m_hostFilters.contains("@local")) return {};
    return *m_hostFilters.cbegin();
}

const SessionsWindow::Entry *SessionsWindow::selected() const
{
    for (const auto &entry : m_entries) if (entry.key == m_selectedKey) return &entry;
    return nullptr;
}

bool SessionsWindow::archiveNameOccupied(const Entry &archive) const
{
    for (const auto &entry : m_entries)
        if (entry.host == archive.host && entry.session.name == archive.session.name && entry.session.state != "archived")
            return true;
    return false;
}

namespace {
QString childState(const QJsonObject &child) {
    return SessionPresentation::childState(child);
}
bool childAction(const QJsonObject &child) {
    const auto state = childState(child);
    // Routine results and failures belong to the parent agent. Escalate only
    // explicit requests for user intervention.
    return state == "approval" || state == "input" || state == "attention";
}
QString childStatus(const QJsonObject &child) {
    return SessionPresentation::childStatus(child);
}
QString childName(const QJsonObject &child, const QString &id) {
    auto name = child.value("label").toString(child.value("name").toString());
    return name.isEmpty() ? id : name;
}
}
QJsonObject SessionsWindow::childRoster(const Entry &entry) const
{
    if (entry.session.subagentSource == "hook_profiles") return {};
    if (entry.key == m_selectedKey && m_details.contains("subagents")
        && (entry.session.conversationId.isEmpty() || entry.session.conversationId == m_details.value("conversation_id").toString())
        && m_details.value("last_event_at").toDouble() >= entry.session.lastEventAt)
        return m_details.value("subagents").toObject();
    return entry.session.subagents;
}
QString SessionsWindow::childKey(const Entry &entry, const QString &id) const
{
    const auto conversation = entry.key == m_selectedKey ? m_details.value("conversation_id").toString(entry.session.conversationId) : entry.session.conversationId;
    return entry.machine + '\n' + entry.session.name + '\n' + conversation + '\n' + id;
}
bool SessionsWindow::showChild(const Entry &entry, const QString &id, const QJsonObject &child, bool countsComplete, bool includeCompleted) const
{
    const auto conversation = !entry.session.conversationId.isEmpty() ? entry.session.conversationId :
        entry.key == m_selectedKey ? m_details.value("conversation_id").toString() : QString();
    const auto draftKey = entry.machine + '\n' + entry.session.name + '\n' + conversation + '\n' + id;
    return includeCompleted
        || (entry.key == m_selectedKey && id == m_subagentId && !id.isEmpty()
            && m_subagentConversation == conversation)
        || (!child.value("group").toBool() && m_subagentComposer->hasDraft(draftKey))
        || !SessionPresentation::inactiveChild(child, countsComplete);
}
bool SessionsWindow::childNeedsAction(const Entry &entry) const
{
    if (!currentActivity(entry.session, entry.online)) return false;
    const auto roster = childRoster(entry);
    for (const auto &child : roster) if (childAction(child.toObject())) return true;
    return false;
}
bool SessionsWindow::entryNeedsAttention(const Entry &entry) const
{
    return SessionPresentation::needsAttention(entry.session, entry.online)
        || (entry.online && !entry.session.attentionAcknowledged && childNeedsAction(entry));
}
bool SessionsWindow::entryHasDraft(const Entry &entry) const
{
    if (entry.session.state == "archived") return false;
    if (m_composer->hasDraft(entry.key)) return true;
    const auto children = childRoster(entry);
    const auto conversation = !entry.session.conversationId.isEmpty() ? entry.session.conversationId :
        entry.key == m_selectedKey ? m_details.value("conversation_id").toString() : QString();
    for (auto it = children.begin(); it != children.end(); ++it)
        if (!it.value().toObject().value("group").toBool()
            && m_subagentComposer->hasDraft(entry.machine + '\n' + entry.session.name + '\n' + conversation + '\n' + it.key())) return true;
    return false;
}
QMap<int, QVariant> SessionsWindow::sessionCardRoles(const Entry &e) const
{
    QMap<int, QVariant> roles;
    const auto &s = e.session;
    const bool includeCompleted = QSettings().value("workspace/showCompletedSubagents", false).toBool();
    const bool terminating=isTerminating(e);
    const QString rowStatus=terminating?tr("Terminating…"):status(s,e.online);
    const QString desc = terminating?tr("Waiting for the session to stop"):currentAction(s, e.online).simplified();
    roles.insert(SessionRoles::Model, s.model.isEmpty() ? (s.cmd == "sh" ? QString() : tr("Model unknown")) : s.model);
    roles.insert(SessionRoles::Effort, s.effort.isEmpty() ? QStringLiteral("—") : s.effort);
    roles.insert(TitleRole, sessionLabel(s)); roles.insert(AgentRole, s.cmd);
    QString meta = SessionPresentation::checkoutLine(s);
    roles.insert(SessionRoles::MetaIcon, SessionPresentation::checkoutIcon(s));
    if (s.state == "archived" && s.archivedAt > 0) {
        if (!meta.isEmpty()) meta += QStringLiteral(" / ");
        meta += QDateTime::fromSecsSinceEpoch(qint64(s.archivedAt)).toLocalTime().toString("d MMM HH:mm");
    }
    roles.insert(MetaRole, meta); roles.insert(StatusRole, rowStatus); roles.insert(DetailRole, desc);
    const auto gitPath = SessionPresentation::gitStatusPath(s);
    const auto git = SessionPresentation::gitStatus(s, m_client.gitStatusSnapshot(e.host, gitPath), e.online);
    roles.insert(SessionRoles::GitHost, e.host); roles.insert(SessionRoles::GitPath, gitPath);
    roles.insert(SessionRoles::GitStatus, git);
    roles.insert(Qt::AccessibleDescriptionRole, GitStatusBadge::tooltip(git));
    roles.insert(SessionRoles::Attention, !terminating && (s.reviewLater || (e.online && !s.attentionAcknowledged && (s.needsAction() || childNeedsAction(e)))));
    roles.insert(SessionRoles::ReviewLater, s.reviewLater);
    roles.insert(SessionRoles::Failure, s.phase=="error");
    roles.insert(SessionRoles::Draft, m_composer->hasDraft(e.key));
    roles.insert(SessionRoles::Unread, !terminating && s.visibleUnreadReply(e.online));
    roles.insert(SessionRoles::Working, !isTerminating(e) && SessionPresentation::working(s, e.online));
    roles.insert(SessionRoles::WorkingSince, SessionPresentation::backgroundIdle(s) ? s.backgroundStarted : s.phase == "compacting" ? s.compactionStarted : s.turnStarted);
    roles.insert(SessionRoles::Scheduled, !isTerminating(e) && scheduled(s, e.online));
    roles.insert(SessionRoles::CoordinationWait, !terminating && SessionPresentation::currentActivity(s,e.online) && s.hasCoordinationWait() ? s.coordinationWait : QJsonObject());
    roles.insert(SessionRoles::Waiting, !terminating && e.online && SessionPresentation::waitingIdle(s));
    roles.insert(SessionRoles::OptionalQuestions, terminating ? 0 : SessionPresentation::optionalQuestionCount(s, e.online));
    roles.insert(SessionRoles::MachineName, e.machine);
    roles.insert(SessionRoles::MachineColor, MachineAppearance::color(e.machine));
    roles.insert(HostRole, e.machine + (e.online ? QString() : tr(" (offline)"))); roles.insert(ChildrenRole, childCount(s, false, e.online, includeCompleted));
    if (const auto *group = m_organization.group(m_organization.groupFor(e.identity))) {
        roles.insert(SessionRoles::ProjectColor, QColor(group->color));
        roles.insert(SessionRoles::ProjectVivid, group->vivid);
    }
    return roles;
}

QMap<int,QVariant> SessionsWindow::childCardRoles(const Entry &e,const QString &id,const QJsonObject &child) const
{
    const auto &s=e.session;
    return {{TitleRole,childName(child,id)},{AgentRole,child.value("provider").toString(s.cmd)},
        {StatusRole,currentActivity(s,e.online)?childStatus(child):tr("Recorded")},
        {DetailRole,child.value("detail").toString(child.value("task").toString()).simplified()},
        {SessionRoles::Working,!isTerminating(e)&&currentActivity(s,e.online)&&childState(child)=="working"},
        {SessionRoles::Attention,currentActivity(s,e.online)&&childAction(child)},
        {SessionRoles::Unread,false},{SessionRoles::ChildId,id},
        {SessionRoles::MachineName,e.machine},{SessionRoles::MachineColor,MachineAppearance::color(e.machine)},{HostRole,e.machine}};
}

void SessionsWindow::rebuild()
{
    if (m_sessions->dragging()) return;
    QSet<QString> selectedKeys;
    for (const auto *item : m_sessions->selectedItems()) selectedKeys.insert(item->data(KeyRole).toString());
    if (!m_sessions->currentItem() && selectedKeys.isEmpty() && !m_selectedKey.isEmpty()) selectedKeys.insert(m_selectedKey);
    m_rebuilding = true;
    QString requestedSelection = m_selectedKey;
    m_entries.clear(); int online = 0, attention = 0, saved = 0, archived = 0, total = 0, active = 0;
    QSet<QString> drafts;
    const bool includeCompleted = QSettings().value("workspace/showCompletedSubagents", false).toBool();
    auto append = [&](const QString &host, const BoxState &box) {
        if (box.host.isEmpty()) return;
        const bool reachable = box.ok && (host.isEmpty() ? m_connectionError.isEmpty() :
            QDateTime::currentMSecsSinceEpoch() - m_fleet.peerPolledAt(host) < FleetState::kPeerStaleMs);
        if (reachable) ++online;
        for (const auto &stored : box.sessions) {
            auto s=stored;
            const QString key = host + '\n' + s.name + (s.state == "archived" ? '\n' + s.archiveId : QString());
            if(key == m_selectedKey && m_inspectError.isEmpty())s=SessionPresentation::inspected(s,m_details);
            m_entries.append({host, host.isEmpty() ? box.host : host, key, {}, s, reachable});
            if (entryHasDraft(m_entries.last())) drafts.insert(key);
        }
    };
    QMap<QString,QString> archiveHosts;
    auto withArchive=[&](const QString &host,const BoxState &box){
        const bool scope=m_hostFilters.isEmpty()||m_hostFilters.contains(host.isEmpty()?"@local":host);
        BoxState view=box;
        if(box.archivePaging){
            view.sessions.removeIf([](const SessionInfo&s){return s.state=="archived";});
            if(box.ok&&scope)archiveHosts[host]=host.isEmpty()?box.host:host;
            view.sessions.append(m_archiveBrowser->sessions(host));
            for(const auto &row:m_archiveSearchRows.value(host)){bool duplicate=false;for(const auto &old:view.sessions)if(old.archiveId==row.archiveId){duplicate=true;break;}if(!duplicate)view.sessions.append(row);}
        }
        append(host,view);
    };
    withArchive({},m_fleet.local());
    for(const auto &peer:m_fleet.peerNames())withArchive(peer,*m_fleet.peer(peer));
    m_archiveBrowser->configure(archiveHosts,m_filter=="archived"&&m_pages->currentIndex()==0);
    updateAttentionIndicator();
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
        if (a.session.state == "archived" && b.session.state == "archived" && a.session.archivedAt != b.session.archivedAt)
            return a.session.archivedAt > b.session.archivedAt;
        if ((a.session.state == "archived") != (b.session.state == "archived")) return a.session.state != "archived";
        return a.key < b.key;
    });
    for (auto &entry : m_entries) entry.identity = m_organization.observe(entry.machine, entry.session.name, entry.session.runId, entry.session.archiveId);
    placeLaunchedSessions();
    saveOrganization();
    // Entries, attention and the organization above stay current for other pages.
    // A hidden Sessions list and its details render once, when the page is shown.
    if (m_pages->currentIndex() != 0 && m_forkKey.isEmpty() && m_restoreKey.isEmpty() && m_renameKey.isEmpty()) {
        m_sessionsNav->setChecked(false); m_machinesNav->setChecked(m_pages->currentIndex() == 2);
        m_sessionsStale = true; m_rebuilding = false; return;
    }
    m_sessionsStale = false;
    if (!m_pending && !m_forkKey.isEmpty()) {
        for (const auto &entry : m_entries) if (entry.key == m_forkKey && entry.session.state == "running") {
            requestedSelection = m_forkKey;
            const auto *group = m_organization.group(m_forkGroup);
            const QString groupId = group ? m_forkGroup : m_organization.defaultProject();
            const QString before = group && group->sessions.contains(m_forkBefore) ? m_forkBefore : QString();
            m_organization.moveSession(entry.identity, groupId, before); m_organization.setCollapsed(groupId, false); saveOrganization();
            const QString key = m_forkKey; m_forkKey.clear(); m_forkGroup.clear(); m_forkBefore.clear();
            QTimer::singleShot(0, this, [this, key] { if (m_selectedKey == key) { m_detailTabs->setCurrentWidget(m_terminal); m_terminal->connectSession(); } });
            break;
        }
    }
    if (!m_pending && !m_restoreKey.isEmpty()) {
        for (const auto &entry : m_entries) if (entry.key == m_restoreKey && entry.session.state == "running") {
            requestedSelection = m_restoreKey; m_restoreKey.clear(); break;
        }
    }
    if (!m_pending && !m_renameKey.isEmpty()) {
        for (const auto &entry : m_entries) if (entry.key == m_renameKey) {
            requestedSelection = m_renameKey; m_renameKey.clear(); break;
        }
    }
    QSet<QString> projectKeys;
    for (const auto &entry : m_entries) {
        if (!m_projectFilters.isEmpty() && !m_projectFilters.contains(m_organization.groupFor(entry.identity))) continue;
        projectKeys.insert(entry.key);
        if (!m_hostFilters.isEmpty() && !m_hostFilters.contains(entry.host.isEmpty() ? "@local" : entry.host)) continue;
        const auto &s = entry.session;
        if (!m_folderFilterPath.isEmpty() && (entry.host != m_folderFilterHost || !MachinePlatform::same(
            m_folderFilterCheckout ? s.gitRoot : s.canonicalCwd, m_folderFilterPath, MachinePlatform::of(m_fleet.local().metrics, entry.host.isEmpty())))) continue;
        if (s.state == "archived") { ++archived; continue; }
        ++total;
        const bool needsAttention = entryNeedsAttention(entry);
        if (needsAttention) ++attention;
        if (needsAttention || SessionPresentation::active(s, entry.online, drafts.contains(entry.key))) ++active;
        if (s.state != "running") ++saved;
    }
    m_heading->setText(m_filter == "archived" ? tr("Archive") : tr("Sessions"));
    const int unreadCount = m_fleet.unreadReplies().size();
    m_markAllReadAction->setEnabled(unreadCount > 0);
    m_markAllReadAction->setText(unreadCount ? tr("Mark all as read (%1)").arg(unreadCount) : tr("Mark all as read"));
    m_markAllReadAction->setIcon(workspaceIcon("read-all", QColor(m_muted)));
    const QMap<QString, int> counts{{"all", total}, {"active", active}, {"attention", attention}, {"paused", saved}, {"archived", archived}};
    QString countCaption;
    for (auto *button : m_filters) {
        const auto id = button->property("filter").toString(); button->setChecked(m_pages->currentIndex() == 0 && id == m_filter);
        if (id == m_filter) countCaption = button->property("caption").toString();
        const bool pagedArchive=id=="archived"&&m_archiveBrowser->paged();
        const bool partial=pagedArchive&&(!m_archiveBrowser->totalsKnown()||m_archiveBrowser->hasMore());
        button->setText(pagedArchive&&!m_archiveBrowser->totalsKnown()&&!counts.value(id)?QStringLiteral("…"):QString::number(counts.value(id))+(partial?QStringLiteral("+"):QString())); button->setProperty("count", counts.value(id));
        button->setToolTip(button->property("caption").toString() + (pagedArchive?tr(" / %1 loaded%2").arg(counts.value(id)).arg(partial?tr("; more can be loaded in Archive"):QString()):QString(" / %1").arg(counts.value(id))));
        button->setIcon(workspaceIcon(id, QColor(id == "attention" && attention > 0 ? (m_dark ? "#f0c77b" : "#885400") : id == m_filter ? m_accent : m_muted)));
    }
    m_sessionsNav->setChecked(m_pages->currentIndex() == 0);
    m_machinesNav->setChecked(m_pages->currentIndex() == 2);
    m_searchResults->setProjectScope(projectKeys, !m_projectFilters.isEmpty());
    m_searchResults->setFolderScope(m_folderFilterHost,m_folderFilterPath,m_folderFilterCheckout);
    m_searchResults->setHostsScope(m_fleet, m_filter, m_hostFilters, drafts);
    m_search->setPlaceholderText(m_filter=="archived"?tr("Search archive names and IDs…"):tr("Search sessions, IDs and messages…"));
    const QString query; // Archive metadata search is paged; other text searches have a separate result list.
    QList<Entry> visible;
    QMap<QString, int> machineCounts, projectCounts;
    for (const auto &e : m_entries) {
        const auto &s = e.session;
        if ((s.state == "archived") != (m_filter == "archived")) continue;
        if(s.state=="archived"&&!m_archiveBrowser->includes(s.archivedAt))continue;
        if (m_filter == "attention" && !entryNeedsAttention(e)
            && !(e.key == m_selectedKey && e.key == m_readSelectionKey)) continue;
        if (m_filter == "active" && !entryNeedsAttention(e) && !SessionPresentation::active(s, e.online, drafts.contains(e.key))
            && !(e.key == m_selectedKey && e.key == m_readSelectionKey)) continue;
        if (m_filter == "paused" && s.state == "running") continue;
        if (!(s.name + ' ' + e.machine + ' ' + s.prompt + ' ' + s.currentTool + ' ' + s.gitBranch + ' ' + s.gitWorktreeName + ' ' + s.activitySummary + ' ' + s.activityDetail).contains(query, Qt::CaseInsensitive)) continue;
        if(!m_folderFilterPath.isEmpty()&&(e.host!=m_folderFilterHost||!MachinePlatform::same(m_folderFilterCheckout?s.gitRoot:s.canonicalCwd,m_folderFilterPath,MachinePlatform::of(m_fleet.local().metrics,e.host.isEmpty()))))continue;
        const QString machine = e.host.isEmpty() ? QStringLiteral("@local") : e.host;
        const auto project = m_organization.groupFor(e.identity);
        if (m_hostFilters.isEmpty() || m_hostFilters.contains(machine)) ++projectCounts[project];
        if (!projectKeys.contains(e.key)) continue;
        ++machineCounts[machine];
        if (!m_hostFilters.isEmpty() && !m_hostFilters.contains(machine)) continue;
        visible.append(e);
    }
    if (!m_folderFilterPath.isEmpty()) countCaption += tr(" / selected folder");
    { const QSignalBlocker block(m_machineFilter); m_machineFilter->setSelection(m_hostFilters); m_machineFilter->setFleet(m_fleet);
        m_machineFilter->setSessionCounts(machineCounts, countCaption + (m_projectFilters.isEmpty() ? QString() : tr(" / selected projects"))); }
    { const QSignalBlocker block(m_projectFilter); m_projectFilter->setSelection(m_projectFilters); m_projectFilter->setProjects(m_organization.groups());
        m_projectFilter->setSessionCounts(projectCounts, countCaption + (m_hostFilters.isEmpty() ? QString() : tr(" / selected machines"))); }
    struct Row { QString group; int entry = -1; bool hidden = false; QString child, launch; };
    QList<Row> rows;
    const bool hasGroups = m_organization.groups().size() > 1;
    const bool filtering = !m_projectFilters.isEmpty() || !m_folderFilterPath.isEmpty() || !query.isEmpty() || !m_hostFilters.isEmpty() || m_filter != "all";
    const bool hideEmptyProjects = QSettings().value("workspace/hideEmptyProjects", true).toBool();
    for (const auto &group : m_organization.groups()) {
        if (!m_projectFilters.isEmpty() && !m_projectFilters.contains(group.id)) continue;
        QList<int> members;
        QStringList launches;
        if (m_filter == "all"&&m_folderFilterPath.isEmpty()) for (auto it = m_pendingLaunches.begin(); it != m_pendingLaunches.end(); ++it) {
            const auto launch = it.value().toObject();
            // Older versions kept mapping receipts for a week. Those abandoned
            // intents must not suddenly become visible launches after an upgrade.
            if (!launch.value("show_pending").toBool()) continue;
            const QString requested = launch.value("group").toString();
            const QString destination = m_organization.group(requested) ? requested : m_organization.defaultProject();
            const QString host = launch.value("host").toString();
            if (destination == group.id && (m_hostFilters.isEmpty() || m_hostFilters.contains(host.isEmpty() ? "@local" : host))) launches.append(it.key());
        }
        for (int i = 0; i < visible.size(); ++i) if (m_organization.groupFor(visible[i].identity) == group.id) members.append(i);
        std::stable_sort(members.begin(), members.end(), [&](int a, int b) {
            return group.sessions.indexOf(visible[a].identity) < group.sessions.indexOf(visible[b].identity);
        });
        if (members.isEmpty() && launches.isEmpty() && (filtering || hideEmptyProjects || !group.accessible)) continue;
        if (hasGroups) rows.append({group.id, -1, false});
        for (int i : members) {
            const bool hidden = hasGroups && group.collapsed && query.isEmpty();
            rows.append({group.id, i, hidden, {}});
            if (m_expandedSessions.contains(visible[i].key)) {
                const auto roster = childRoster(visible[i]);
                // Stable ID order avoids moving a row under the pointer when a child finishes.
                for (auto it = roster.begin(); it != roster.end(); ++it)
                    if (showChild(visible[i], it.key(), it.value().toObject(), visible[i].session.subagentCountsComplete, includeCompleted))
                        rows.append({group.id, i, hidden, it.key()});
            }
        }
        for (const auto &id : launches) rows.append({group.id, -1, hasGroups && group.collapsed, {}, id});
    }
    const auto rowKey = [&](const Row &row) { return !row.launch.isEmpty() ? "#launch\n" + row.launch : row.entry < 0 ? "#group\n" + row.group : visible[row.entry].key + (row.child.isEmpty() ? QString() : "\n#child\n" + row.child); };
    bool sameRows = m_sessions->count() == rows.size();
    for (int i = 0; sameRows && i < rows.size(); ++i) sameRows = m_sessions->item(i)->data(KeyRole).toString() == rowKey(rows[i]);
    const int scroll = m_sessions->verticalScrollBar()->value();
    if (!sameRows) m_sessions->clear();
    int selectedRow = -1, firstSession = -1, firstVisible = -1;
    for (int i = 0; i < rows.size(); ++i) {
        const auto row = rows[i];
        auto *item = sameRows ? m_sessions->item(i) : new QListWidgetItem(m_sessions);
        // Session cards already expose their status; details belong in Activity
        // and Details, not a hover popup covering neighbouring rows.
        item->setToolTip({});
        item->setData(KeyRole, rowKey(row)); item->setData(SessionRoles::Group, row.group);
        item->setData(SessionRoles::ChildId, row.child);
        item->setData(SessionRoles::ParentKey, row.entry >= 0 ? visible[row.entry].key : QString());
        item->setData(SessionRoles::LaunchId, row.launch);
        item->setData(SessionRoles::WorkingSince, 0.0);
        item->setData(SessionRoles::Scheduled, false);
        item->setData(SessionRoles::OptionalQuestions, 0);
        item->setData(SessionRoles::GitStatus, QJsonObject());
        item->setData(SessionRoles::MetaIcon, QString());
        item->setData(SessionRoles::GitHost, QString()); item->setData(SessionRoles::GitPath, QString());
        item->setData(Qt::AccessibleDescriptionRole, QString());
        item->setData(SessionRoles::Header, row.entry < 0 && row.launch.isEmpty()); item->setHidden(row.hidden);
        if (!row.launch.isEmpty()) {
            const auto launch = m_pendingLaunches.value(row.launch).toObject();
            const QString name = launch.value("tag").toString(), host = launch.value("host").toString();
            const QString machine = host.isEmpty() ? m_fleet.local().host : host;
            const bool waiting = QDateTime::currentSecsSinceEpoch() - launch.value("created").toInteger() > 120;
            const QString state = waiting ? tr("Waiting for session") : tr("Starting…");
            const QString detail = waiting ? tr("Check the terminal for startup progress") : tr("Waiting for the session to appear");
            item->setFlags(Qt::ItemIsEnabled);
            item->setData(TitleRole, name); item->setData(StatusRole, state); item->setData(DetailRole, detail);
            item->setData(MetaRole, launch.value("target").toString()); item->setData(AgentRole, launch.value("cmd").toString());
            item->setData(HostRole, machine); item->setData(SessionRoles::MachineName, machine); item->setData(SessionRoles::MachineColor, MachineAppearance::color(machine));
            item->setData(Qt::AccessibleTextRole, QString("%1, %2, %3").arg(name, machine, state));
            continue;
        }
        if (row.entry < 0) {
            const auto *group = m_organization.group(row.group);
            int total = 0, alerts = 0, busy = 0, unread = 0;
            for (const auto &e : visible) if (m_organization.groupFor(e.identity) == row.group) ++total;
            for (const auto &candidate : rows) if (candidate.group == row.group && !candidate.launch.isEmpty()) ++total;
            // Attention remains visible even when a search or state filter hides a member.
            for (const auto &e : m_entries) if (m_organization.groupFor(e.identity) == row.group) {
                if (e.session.reviewLater || (e.online && !e.session.attentionAcknowledged && (e.session.needsAction() || childNeedsAction(e)))) ++alerts;
                if (e.session.visibleUnreadReply(e.online)) ++unread;
                if (SessionPresentation::working(e.session, e.online)) ++busy;
            }
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled);
            item->setData(TitleRole, group->name);
            item->setData(SessionRoles::ProjectColor, QColor(group->color));
            item->setData(SessionRoles::ProjectVivid, group->vivid);
            item->setData(SessionRoles::Collapsed, group->collapsed && query.isEmpty());
            item->setData(SessionRoles::Total, total); item->setData(SessionRoles::Attention, alerts); item->setData(SessionRoles::Working, busy); item->setData(SessionRoles::Unread, unread);
            const QString description = tr("%1: %2 shown, %3 need a response, %4 working, %5 unread replies. Click to collapse or expand; drag to reorder.")
                .arg(group->name).arg(total).arg(alerts).arg(busy).arg(unread);
            item->setToolTip(sessionTooltip({description}, 330).html); item->setData(Qt::AccessibleTextRole, description);
            continue;
        }
        const auto &e = visible[row.entry]; const auto &s = e.session;
        if (!row.child.isEmpty()) {
            const auto child = childRoster(e).value(row.child).toObject();
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            const auto roles=childCardRoles(e,row.child,child);for(auto it=roles.cbegin();it!=roles.cend();++it)item->setData(it.key(),it.value());
            item->setData(Qt::AccessibleTextRole, tr("Subagent %1, %2").arg(childName(child, row.child), childStatus(child)));
            if (e.key == requestedSelection && row.child == m_subagentId) selectedRow = i;
            continue;
        }
        const auto roster = childRoster(e);
        bool hasChildren = false;
        for (auto it = roster.begin(); it != roster.end(); ++it)
            if (showChild(e, it.key(), it.value().toObject(), s.subagentCountsComplete, includeCompleted)) { hasChildren = true; break; }
        item->setData(SessionRoles::HasChildren, hasChildren);
        item->setData(SessionRoles::Expanded, m_expandedSessions.contains(e.key));
        if (firstSession < 0) firstSession = i;
        if (!row.hidden && firstVisible < 0) firstVisible = i;
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        item->setData(SessionRoles::Identity, e.identity);
        const auto roles = sessionCardRoles(e);
        for (auto role = roles.cbegin(); role != roles.cend(); ++role) item->setData(role.key(), role.value());
        const auto rowStatus = roles.value(StatusRole).toString(), desc = roles.value(DetailRole).toString();
        const auto meta = roles.value(MetaRole).toString(), gitPath = roles.value(SessionRoles::GitPath).toString();
        if (e.online && !gitPath.isEmpty()) m_client.requestGitStatus(e.host, gitPath);
        QStringList accessibleFields{s.name, e.machine + QStringLiteral(" / ") + s.cmd + QStringLiteral(" / ") + rowStatus, meta};
        if (item->data(SessionRoles::Working).toBool() && item->data(SessionRoles::WorkingSince).toDouble() > 0)
            accessibleFields << tr("%1 since %2").arg(SessionPresentation::backgroundIdle(s) ? tr("Background") : s.phase == "compacting" ? tr("Compacting") : tr("Working"), QDateTime::fromSecsSinceEpoch(qint64(item->data(SessionRoles::WorkingSince).toDouble())).toLocalTime().toString("d MMM HH:mm:ss"));
        if (!s.cwd.isEmpty()) accessibleFields << s.cwd;
        accessibleFields << (s.model + (s.effort.isEmpty() ? QString() : "  " + s.effort));
        if (s.visibleUnreadReply(e.online)) accessibleFields << tr("New reply (not viewed yet)");
        const int optionalQuestions = roles.value(SessionRoles::OptionalQuestions).toInt();
        if (optionalQuestions > 0) accessibleFields << SessionPresentation::optionalQuestionsLabel(optionalQuestions);
        if (s.reviewLater) accessibleFields << tr("Marked needs attention: review later");
        accessibleFields << desc;
        if (!childCount(s, false, e.online, includeCompleted).isEmpty()) accessibleFields << tr("Subagents: %1").arg(childCount(s, true, e.online, includeCompleted));
        if (!currentActivity(s, e.online) && !s.currentTool.isEmpty()) accessibleFields << tr("Last recorded: %1").arg(s.currentTool + " / " + s.toolDetail);
        if (s.gitMetadataState == "unavailable") accessibleFields << tr("Git information unavailable");
        const int availableWidth = m_sessions->screen() ? m_sessions->screen()->availableGeometry().width() : 460;
        item->setData(Qt::AccessibleTextRole, sessionTooltip(accessibleFields, qBound(160, availableWidth - 40, 420)).plain);
        if (e.key == requestedSelection) selectedRow = i;
    }
    // List scopes do not choose a replacement for a conversation that still exists.
    const bool preserveConversation = selected();
    // Choose a first row only on initial load. A removed conversation leaves an
    // empty selection until an explicit choice, even after later fleet updates.
    if (!m_selectedKey.isEmpty()) m_initialSelectionPending = false;
    if (selectedRow < 0 && !preserveConversation && m_initialSelectionPending) selectedRow = firstVisible < 0 ? firstSession : firstVisible;
    if (selectedRow >= 0) m_initialSelectionPending = false;
    if (selectedRow >= 0 && m_sessions->item(selectedRow)->data(KeyRole).toString() == m_selectedKey) {
        m_sessions->setCurrentRow(selectedRow, QItemSelectionModel::NoUpdate);
        QItemSelection selection;
        for (int i = 0; i < m_sessions->count(); ++i) {
            auto *item = m_sessions->item(i);
            if (!item->isHidden() && selectedKeys.contains(item->data(KeyRole).toString())) {
                const auto index = m_sessions->model()->index(i, 0); selection.select(index, index);
            }
        }
        m_sessions->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    } else {
        m_sessions->clearBulkSelection(); m_sessions->setCurrentRow(selectedRow);
    }
    m_sessions->verticalScrollBar()->setValue(scroll);
    int visibleWorking = 0, visibleAttention = 0;
    for (const auto &e : visible) {
        if (entryNeedsAttention(e)) ++visibleAttention;
        if (SessionPresentation::working(e.session, e.online)) ++visibleWorking;
    }
    m_count->setText(visible.isEmpty() ? tr("No matching sessions") : tr("%1 shown, %2 working, %3 attention").arg(visible.size()).arg(visibleWorking).arg(visibleAttention));
    const int starting = std::count_if(rows.cbegin(), rows.cend(), [](const Row &row) { return !row.launch.isEmpty(); });
    if (starting) m_count->setText(visible.isEmpty() ? tr("%1 starting").arg(starting) : m_count->text() + tr(", %1 starting").arg(starting));
    m_countFull = m_count->text(); m_countShort = QString::number(visible.size() + starting);
    // The strip keeps this row hidden (applySessionStrip()).
    m_folderFilterClear->setVisible(!m_folderFilterPath.isEmpty() && !(m_sessionDock && m_sessionDock->chromeNarrow()));
    const auto folderStyle=MachinePlatform::of(m_fleet.local().metrics,m_folderFilterHost.isEmpty());
    m_folderFilterClear->setText(tr("Folder: %1   ×").arg(MachinePlatform::leaf(m_folderFilterPath,folderStyle)));m_folderFilterClear->setToolTip(MachinePlatform::native(m_folderFilterPath,folderStyle)+tr("\nClear folder filter"));
    m_count->setToolTip(m_countFull);
    m_count->setVisible(!m_searchResults->active());
    applySessionStrip();
    m_rebuilding = false;
    updateSelectionActions();
    m_sessions->syncActivityAnimation();
    // A vanished target also clears while search results own the list.
    if ((selectedRow < 0 && !preserveConversation) || (selectedRow >= 0 && !m_searchResults->active())) selectSession(); else renderDetails();
}

void SessionsWindow::updateAttentionIndicator()
{
    m_attentionCount = std::count_if(m_entries.cbegin(),m_entries.cend(),[this](const Entry &entry) {
        return entry.session.state != "archived" && entryNeedsAttention(entry);
    });
    m_attentionBadge->setText(m_attentionCount > 99 ? "99+" : QString::number(m_attentionCount));
    setWorkspaceStyle(m_attentionBadge, QString("QLabel{background:%1;color:%2;border-radius:7px;font-size:9px;font-weight:700;}")
        .arg(m_dark ? "#ffda76" : "#a66000",m_dark ? "#30250c" : "#ffffff"));
    m_attentionBadge->setVisible(m_attentionCount > 0);
    const QString hint = m_attentionCount ? tr("Sessions — %1 need attention").arg(m_attentionCount) : tr("Sessions");
    m_sessionsNav->setAccessibleName(hint); m_sessionsNav->setToolTip(ShortcutRegistry::instance().tooltip(hint, "go.sessions"));
    m_brand->setProperty("attentionCount",m_attentionCount);
    m_brand->setAppearance(m_dark ? QColor(Qt::white) : QColor(m_accent), QColor(m_dark ? "#ffda76" : "#a66000"),
        m_attentionCount > 0, QSettings().value("workspace/reduceMotion", false).toBool());
}

void SessionsWindow::markAllRepliesRead()
{
    const auto replies = m_fleet.unreadReplies();
    if (replies.isEmpty()) return;
    // Keep the open conversation and its draft in place, even if the attention
    // filter would otherwise remove this session after acknowledgement.
    if (const auto *entry = selected(); entry && entry->session.unreadReply) m_readSelectionKey = entry->key;
    m_readCandidate.clear();
    const int count = m_fleet.markRepliesRead(replies);
    ReadMarkStore::add(m_fleet.takeNewReadMarks());
    if (!count) return;
    rebuild(); updateDashboard(); emit repliesMarkedRead(replies);
    showNotice(count == 1 ? tr("1 reply marked as read.") : tr("%1 replies marked as read.").arg(count));
}

void SessionsWindow::checkViewedReply()
{
    const auto *entry = selected();
    const bool active = isVisible() && isActiveWindow() && !isMinimized() && m_pages->currentIndex() == 0
        && !QApplication::activeModalWidget() && !QApplication::activePopupWidget();
    if (!m_subagentId.isEmpty()) { m_readCandidate.clear(); return; }
    bool latest = false;
    if (active && entry && entry->session.unreadReply && entry->online && m_inspectError.isEmpty()
        && m_details.value("conversation_id").toString() == entry->session.conversationId
        && m_details.value("reply_id").toString() == entry->session.replyId) {
        if (m_activityView->isVisible()) latest = m_activityView->replyVisible(entry->session.replyId);
        else if (m_terminal->isVisible() && m_terminal->isConnected()) {
            const auto *screen = m_terminal->terminalScreen(); const auto *scroll = screen->verticalScrollBar();
            latest = scroll->value() == scroll->maximum() && !screen->screenText().trimmed().isEmpty();
        }
    }
    if (!latest) { m_readCandidate.clear(); return; }
    const QString candidate = entry->key + '\n' + entry->session.conversationId + '\n' + entry->session.replyId;
    if (candidate != m_readCandidate) { m_readCandidate = candidate; m_readDwell.start(); return; }
    if (m_readDwell.elapsed() < 1000) return;
    const auto host = entry->host, name = entry->session.name, conversation = entry->session.conversationId, reply = entry->session.replyId;
    // Keep the opened result in the attention view until the user switches away.
    // Acknowledging it must not replace the conversation under their eyes.
    m_readSelectionKey = entry->key; m_readCandidate.clear();
    if (m_fleet.markReplyRead(host, name, conversation, reply)) {
        ReadMarkStore::add(m_fleet.takeNewReadMarks());
        rebuild(); updateDashboard(); emit replyViewed(host, name, conversation, reply);
    }
}

void SessionsWindow::selectSession()
{
    const auto *item = m_sessions->currentItem();
    if (item && (item->data(SessionRoles::Header).toBool() || !item->data(SessionRoles::LaunchId).toString().isEmpty())) return;
    const QString child = item ? item->data(SessionRoles::ChildId).toString() : QString();
    const QString key = item ? item->data(child.isEmpty() ? KeyRole : SessionRoles::ParentKey).toString() : QString();
    if (key != m_selectedKey) {
        pushSelectionChanged(key);
        m_readCandidate.clear(); m_readSelectionKey.clear();
        m_selectedKey = key; m_details = {}; m_events = {}; m_cursor = 0; m_inspectError.clear();
        rememberRecentSession(key);
        m_processPollAge.invalidate();m_inspectionAge.invalidate();
        m_childrenHtml.clear(); m_activityView->setSessionKey(key); m_composer->setSessionKey(key);
        const auto *entry = selected();
        m_terminal->setSession(m_client.executable(), entry ? entry->host : QString(), entry ? entry->session.name : QString(), {}, entry ? entry->machine : QString());
        if (!child.isEmpty()) openSubagent(child);
        inspect(); renderDetails();
        if (selected() && m_subagentId.isEmpty() && m_detailTabs->currentWidget() == m_terminal) m_terminal->connectSession();
        return;
    }
    if (!child.isEmpty() && child != m_subagentId) openSubagent(child);
    else if (child.isEmpty() && !m_subagentId.isEmpty()) closeSubagent();
    renderDetails();
}

void SessionsWindow::inspect()
{
    if (!m_pending && !m_renameKey.isEmpty()) return;
    const auto *entry = selected();
    if(isVisible() && m_pages->currentIndex()==3)updateDashboardAccounts(true);
    if (!isVisible() || m_pages->currentIndex() != 0 || !entry || !entry->online) return;
    const int processInterval=m_detailTabs->currentWidget()==m_processes?ProcessSettings::activeMs():ProcessSettings::backgroundMs();
    const bool processes=ProcessSettings::enabled() && (!m_processPollAge.isValid() || m_processPollAge.elapsed()>=processInterval);
    if(m_client.requestInspection(entry->host, entry->session.name, m_cursor, entry->session.archiveId,processes)) {
        m_inspectionAge.start();if(processes)m_processPollAge.start();
    }
    refreshAccountUsage();
    if (!m_subagentId.isEmpty()) m_client.requestSubagentInspection(entry->host, entry->session.name, m_subagentId, entry->session.archiveId);
}

void SessionsWindow::renderAccountUsage()
{
    const auto *entry=selected();const bool visible=m_pages->currentIndex()==0 && entry && entry->session.tracked && entry->session.archiveId.isEmpty() && m_subagentId.isEmpty();
    // Hide only when it should go: hiding and showing again would close an open popover on every refresh.
    m_usageStrip->setVisible(visible);if(!visible){m_recovery->setUsageNotice({});m_usageLimit->setActive(false);m_usageRefresh->setRefreshing(false);return;}
    m_accountUsageData=m_usageStore->data(AccountUsageRef::bound(entry->host,entry->session));m_accountUsage->setData(m_accountUsageData);
    QString notice;
    const auto failure=m_details.value("provider_error").toObject();
    if(entry->online && entry->session.cmd=="claude" && m_details.value("run_id").toString()==entry->session.runId
        && m_details.value("conversation_id").toString()==entry->session.conversationId
        && m_details.value("provider_status_at").toDouble()>=entry->session.providerStatusAt
        && (m_details.value("phase")=="error" || m_details.value("attention_id").toString().startsWith("claude-usage-limit:"))
        && (failure.value("error_kind")=="quota" || failure.value("error_kind")=="session_limit"))
        notice=AccountUsage::limitRecoveryNotice(m_accountUsageData,failure.value("at").toDouble());
    m_recovery->setUsageNotice(notice);
    const auto exhausted=AccountUsage::exhausted(m_accountUsageData);const auto warning=exhausted.isEmpty()?notice:exhausted;m_usageWarning->setText(warning);
    m_usageLimit->setTone(exhausted.isEmpty()?ChipTone::Neutral:ChipTone::Danger);
    m_usageLimit->setLabels(exhausted.isEmpty()?tr("Usage available"):AccountUsage::exhaustedSummary(m_accountUsageData),exhausted.isEmpty()?tr("Usage"):tr("Limit"));m_usageLimit->setDetail(warning);
    m_usageLimit->setActive(!warning.isEmpty());
    m_usageRefresh->setRefreshing(m_accountUsageData["refreshing"].toBool());
    m_usageRefresh->setEnabled(entry->online && !m_accountUsageData["refreshing"].toBool());
}
void SessionsWindow::refreshAccountUsage(bool force)
{
    const auto *entry=selected();
    if(entry && entry->online && entry->session.tracked && entry->session.archiveId.isEmpty() && m_subagentId.isEmpty())
        m_usageStore->ensure(AccountUsageRef::bound(entry->host,entry->session),force);
    renderAccountUsage();
}
void SessionsWindow::updateDashboardAccounts(bool request,bool force)
{
    // Showing Overview requests and renders accounts again; skip hidden updates.
    if(!isVisible() || m_pages->currentIndex()!=3)return;
    if(request)m_accountsPage->ensureCatalogs(force);
    QJsonArray rows;
    for(const auto &value:m_accountsPage->profiles()) {
        auto p=value.toObject();const auto ref=AccountUsageRef::profile(p);
        if(request && p["installed"].toBool())m_usageStore->ensure(ref,force);
        rows.append(p);
    }
    m_dashboard->setAccounts(rows);
}

void SessionsWindow::openSubagent(const QString &id)
{
    const auto *entry = selected();
    const auto roster = entry ? childRoster(*entry) : QJsonObject();
    if (!entry || !roster.contains(id)) return;
    if (m_subagentId == id) return;
    m_subagentDetails = {}; m_subagentComposer->setInputVisible(false);
    m_subagentComposer->setSessionKey(childKey(*entry, id));
    m_subagentId = id; m_subagentConversation = m_details.value("conversation_id").toString(entry->session.conversationId);
    m_subagentCwd = selectedDirectory();
    m_subagentTitle->setText(childName(roster.value(id).toObject(), id));
    m_subagentTitle->setToolTip(id);
    m_subagentHint->setText(tr("Loading subagent activity…"));
    m_subagentView->setSessionKey(m_selectedKey + '\n' + m_subagentConversation + '\n' + id);
    m_subagentView->setActivity({},{});
    m_activityStack->setCurrentIndex(1); m_detailTabs->setCurrentIndex(0);
    m_expandedSessions.insert(entry->key);
    rebuild(); inspect();
}

void SessionsWindow::closeSubagent()
{
    const bool wasOpen = !m_subagentId.isEmpty();
    m_subagentId.clear(); m_subagentConversation.clear(); m_subagentCwd.clear(); m_subagentDetails = {};
    m_activityStack->setCurrentIndex(0);
    const QSignalBlocker block(m_sessions);
    for (int i = 0; i < m_sessions->count(); ++i) if (m_sessions->item(i)->data(KeyRole).toString() == m_selectedKey) { m_sessions->setCurrentRow(i); break; }
    if (!m_rebuilding) renderDetails();
    // Drop the temporary visible-row pin after deliberate navigation, outside
    // selection/render callbacks that still hold pointers into m_entries.
    if (wasOpen && !m_rebuilding) QTimer::singleShot(0, this, [this]{ rebuild(); });
}

void SessionsWindow::beginSendEvidence(const QString &key, const QJsonObject &data)
{
    ++m_pendingEvidenceUsers[key];
    if (!data.isEmpty()) m_pendingSendEvidence.insert(key, sendEvidence(data));
    else if (!m_pendingSendEvidence.contains(key)) m_pendingSendEvidence.insert(key, {});
}

void SessionsWindow::finishSendEvidence(const QString &key)
{
    if (--m_pendingEvidenceUsers[key] > 0) return;
    m_pendingEvidenceUsers.remove(key); m_pendingSendEvidence.remove(key);
}

void SessionsWindow::acceptInspection(const QString &host, const QString &name, const QJsonObject &reply, const QString &archiveId)
{
    auto data = reply;
    if (archiveId.isEmpty()) {
        for (const auto &entry : m_entries)
            if (entry.host == host && entry.session.name == name && entry.session.archiveId.isEmpty())
                if (m_pendingSendEvidence.contains(entry.key)) m_pendingSendEvidence.insert(entry.key, sendEvidence(data));
        continueAfterCompact(host, name, data); continueAfterStart(host, name, data);
    }
    if (!m_pending && !m_renameKey.isEmpty()) return;
    const auto *entry = selected(); if (!entry || entry->host != host || entry->session.name != name || entry->session.archiveId != archiveId) return;
    // A reply read before a settings receipt of the same run would undo it: drop
    // its pending change, so the queued automatic apply never comes, or bring back
    // one the receipt applied. Keep the receipt's settings; the next poll reads anew.
    if (m_inspectionBehindSettings.remove(entry->key) && !m_details.isEmpty()
        && m_details.value("run_id") == data.value("run_id") && m_details.value("conversation_id") == data.value("conversation_id"))
        for (const auto *field : {"model", "effort", "pending_model", "pending_effort", "pending_settings_id"}) {
            if (m_details.contains(field)) data[field] = m_details.value(field);
            else data.remove(field);
        }
    // A confirmed clear continues the same Activity timeline, and its global
    // journal cursor stays valid. Keep the earlier events in place.
    if (!m_details.isEmpty() && m_details.value("conversation_id") != data.value("conversation_id")
        && !data.value("cleared_conversations").toArray().contains(m_details.value("conversation_id"))) {
        // The conversation changed under this terminal. Fetch its initial window;
        // a cursor from the previous conversation must never skip the new history.
        m_events = data.value("events").toArray(); m_cursor = 0; m_details = data; m_processPollAge.invalidate();
        reconcileMessages(); renderDetails(); pushInspected(entry->key, m_details); inspect(); return;
    }
    const auto previousRoster = childRoster(*entry);
    const auto inspected=SessionPresentation::inspected(entry->session,data);
    const bool statusChanged=inspected.phase!=entry->session.phase || inspected.activity!=entry->session.activity
        || inspected.providerError!=entry->session.providerError || inspected.recovery!=entry->session.recovery;
    const auto previousProcesses=m_details.value("processes");
    const bool sameRun=m_details.value("run_id")==data.value("run_id") && m_details.value("host_generation")==data.value("host_generation");
    m_details = data; m_inspectError.clear();
    if(!ProcessSettings::enabled())m_details.remove("processes");
    else if(!data.contains("processes") && sameRun && previousProcesses.isObject())m_details["processes"]=previousProcesses;
    else if(!data.contains("processes"))m_processPollAge.invalidate();
    if (data.value("replace_events").toBool()) m_events = data.value("events").toArray();
    else {
        // A conversation switch resets the fetch cursor while retaining its
        // first window. The full refetch must not append those events again.
        QSet<qint64> knownSequences;
        for (const auto &event : m_events) knownSequences.insert(event.toObject().value("seq").toInteger());
        for (const auto &event : data.value("events").toArray()) {
            const auto seq = event.toObject().value("seq").toInteger();
            if (seq <= m_cursor || knownSequences.contains(seq)) continue;
            knownSequences.insert(seq); m_events.append(event);
        }
    }
    while (m_events.size() > 500) m_events.removeFirst();
    m_cursor = data.value("cursor").toInteger(); reconcileMessages(); renderDetails(); applyPendingModelSettings();
    pushInspected(entry->key, m_details);
    if (statusChanged || previousRoster != childRoster(*entry)) rebuild();
}

namespace {
// Why the host reports a native context action ("clear", "compact")
// unavailable. Older hosts omit the reason: the action keeps its usual text.
QString contextActionReason(const QJsonObject &details, const QString &action)
{
    if (details.value(action + "_context_supported").toBool()) return {};
    return details.value(action + "_context_reason").toString().simplified();
}
QString contextReasonSentence(const QString &reason)
{
    return reason.isEmpty() || QStringLiteral(".!?…").contains(reason.back()) ? reason : reason + '.';
}
// A disabled context action shows that reason instead of its usual tooltip.
template<typename Control> void explainContextAction(Control *control, const QString &reason)
{
    if (!control->property("usualToolTip").isValid()) control->setProperty("usualToolTip", control->toolTip());
    // Reported names stay literal, as in the cold-cache line.
    control->setToolTip(reason.isEmpty() ? control->property("usualToolTip").toString()
        : Qt::convertFromPlainText(contextReasonSentence(reason), Qt::WhiteSpaceNormal));
}
}

void SessionsWindow::renderDetails()
{
    updateSessionLoading();
    m_compactCancel->setVisible(m_compacts.contains(m_selectedKey) || (m_startSend.key == m_selectedKey && !m_startSend.key.isEmpty()));
    const auto *entry = selected(); m_detailStack->setCurrentIndex(entry ? 1 : 0);
    const auto recoveryDetails=entry && entry->session.state != "archived"
        && m_details.value("provider_status_at").toDouble() >= entry->session.providerStatusAt ? m_details : QJsonObject();
    const auto recoveryState=recoveryDetails.value("recovery").toObject().value("state").toString();
    const bool providerFailure=recoveryDetails.value("phase")=="error" && !recoveryDetails.value("provider_error").toObject().isEmpty();
    auto *recoveryFocus=QApplication::focusWidget();
    if(!providerFailure&&(recoveryState.isEmpty()||recoveryState=="succeeded")&&m_recovery->isVisible()&&recoveryFocus
        &&(recoveryFocus==m_recovery||m_recovery->isAncestorOf(recoveryFocus)))m_composer->editor()->setFocus(Qt::OtherFocusReason);
    m_recovery->setState(recoveryDetails,entry && entry->online);
    m_markRead->setActive(entry && entry->session.state!="archived" && entryNeedsAttention(*entry));
    m_markRead->setDetail(entry && entry->session.reviewLater
        ? tr("Marked for later. Mark as read to clear this reminder.")
        : tr("Mark this session's current notification as read. This does not answer a pending question."));
    renderAccountUsage();
    updateWorktrees();
    const bool recorded=!entry || !entry->online || entry->session.state=="archived" || entry->session.state=="paused" || entry->session.processState=="exited" || !m_inspectError.isEmpty();
    if(ProcessSettings::enabled())m_processes->setSession(entry?entry->host:QString(),entry?entry->session.name:QString(),entry?entry->session.archiveId:QString(),entry?m_details:QJsonObject(),entry&&entry->online,!recorded);
    const auto processData=m_details.value("processes").toObject();
    const int processCount=recorded?0:qMax(processData.value("live_count").toInt(),processData.value("active_count").toInt());
    m_detailTabs->setTabText(m_detailTabs->indexOf(m_processes),processCount>0?tr("Processes (%1)").arg(processCount):tr("Processes"));
    m_contextUsage->setData(entry?m_details.value("session_usage").toObject():QJsonObject(),recorded);
    m_cacheStatus->setData(entry?m_details:QJsonObject(),recorded);
    const auto cacheWarning=entry?CacheStatus::warning(m_details):QString();m_cacheWarning->setText(cacheWarning);m_cacheWarning->setVisible(!cacheWarning.isEmpty());
    m_cacheDetail->setText(CacheStatus::details(m_details,recorded));m_cacheClear->setVisible(!cacheWarning.isEmpty());
    m_cacheClear->setEnabled(entry && entry->online && !m_clearRequest && m_details.value("clear_context_supported").toBool());
    // An offline host's last inspection no longer explains the action.
    const bool reported=entry && entry->online;
    explainContextAction(m_cacheClear,reported?contextActionReason(m_details,"clear"):QString());
    const bool mainContext=m_subagentId.isEmpty();
    m_detailsClear->setVisible(mainContext);m_detailsContext->setVisible(mainContext);
    const bool canClear=mainContext && entry && entry->online && entry->session.state!="archived"
        && !m_clearRequest && !m_composer->isSending(m_selectedKey) && m_inspectError.isEmpty()
        && !m_details.value("run_id").toString().isEmpty() && !m_details.value("conversation_id").toString().isEmpty()
        && (m_details.value("clear_context_supported").toBool() || m_details.value("clear_context_dismiss").toBool());
    m_detailsClear->setEnabled(canClear);
    // A Claude chooser still allows Dismiss and clear.
    const auto clearReason=reported && !m_details.value("clear_context_dismiss").toBool()?contextActionReason(m_details,"clear"):QString();
    explainContextAction(m_detailsClear,clearReason);explainContextAction(m_clearAction,clearReason);
    m_detailsClear->setText(m_clearRequest?tr("Clearing…"):tr("Clear session"));
    // A live agent clears natively; a stopped one starts fresh instead.
    const bool stopped=entry && (entry->session.state=="paused" || entry->session.state=="stopped");
    m_clearAction->setVisible(mainContext && !stopped);m_clearAction->setEnabled(canClear);
    m_clearAction->setText(m_clearRequest?tr("Clearing…"):tr("Clear session…"));
    m_freshAction->setVisible(mainContext && stopped);m_freshAction->setEnabled(entry && canStartStopped(*entry));
    m_detailsContext->setText(tr("Context: %1").arg(m_contextUsage->text().isEmpty()?tr("Not reported"):m_contextUsage->text()));m_detailsContext->setToolTip(m_contextUsage->toolTip());
    m_subagentContextUsage->setData(entry?m_subagentDetails.value("session_usage").toObject():QJsonObject(),recorded);
    if (!m_subagentId.isEmpty() && (!entry || (m_subagentConversation != m_details.value("conversation_id").toString(entry->session.conversationId))
        || !childRoster(*entry).contains(m_subagentId))) closeSubagent();
    if (!entry) {
        m_hint->clear(); m_hint->setProperty("active", false); m_hint->hide();
        m_composer->setSessionStatus({}); updateSessionFeedback();
        m_detailsIdValue->setText(tr("Not confirmed")); m_detailsIdValue->setProperty("copyValue", QString());
        m_copySessionId->setEnabled(false);
        m_question->setQuestion({}, {}); m_question->hide();
        m_terminal->setAvailable(false, tr("Select a running session."));
        m_composer->setAvailability(false, tr("Select a session.")); return;
    }
    auto s = entry->session;
    if (m_details.value("tracked").toBool() && m_details.value("last_event_at").toDouble() >= s.lastEventAt
        && m_details.value("provider_status_at").toDouble() >= s.providerStatusAt) {
        s.phase = m_details.value("phase").toString(s.phase); s.activity = m_details.value("activity").toString(s.activity);
        s.processState = m_details.value("process_state").toString(s.processState);
        s.conversationState = m_details.value("conversation_state").toString(s.conversationState);
        s.runtimeState = m_details.value("runtime_state").toString(s.runtimeState);
        s.activitySummary = m_details.value("activity_summary").toString(s.activitySummary);
        s.activityDetail = m_details.value("activity_detail").toString(s.activityDetail);
        s.providerError = m_details.value("provider_error").toObject();
        s.currentTool = m_details.value("current_tool").toString(s.currentTool);
        s.toolDetail = m_details.value("tool_detail").toString(s.toolDetail);
    }
    s.cwd = m_details.value("cwd").toString(s.cwd);
    const QString directory = m_subagentId.isEmpty() ? s.cwd : m_subagentCwd;
    m_shell->setEnabled(entry->online && MachinePlatform::absolute(directory, MachinePlatform::of(m_fleet.local().metrics, entry->host.isEmpty())));
    const bool localFolder = entry->host.isEmpty() && QFileInfo(directory).isAbsolute() && QFileInfo(directory).isDir();
    m_fileManager->setEnabled(localFolder);
    m_fileManager->setToolTip(!entry->host.isEmpty() ? tr("Available only for sessions on this computer")
        : localFolder ? tr("Open session folder in file manager\n%1").arg(directory) : tr("Session folder is unavailable"));
    s.gitRoot = m_details.value("git_root").toString(s.gitRoot);
    s.gitBranch = m_details.value("git_branch").toString(s.gitBranch);
    s.gitWorktree = m_details.value("git_worktree").toBool(s.gitWorktree);
    s.gitWorktreeName = m_details.value("git_worktree_name").toString(s.gitWorktreeName);
    s.gitCommonDir = m_details.value("git_common_dir").toString(s.gitCommonDir);
    s.gitDetached = m_details.value("git_detached").toBool(s.gitDetached);
    const bool terminating=isTerminating(*entry);
    const QString state = terminating?tr("Terminating…"):status(s, entry->online);
    const auto *project = m_organization.group(m_organization.groupFor(entry->identity));
    m_fullTitle = project ? project->name + " / " + sessionLabel(s) : displayTitle(s); m_title->setToolTip(sessionTooltip({s.name}, 380).html);
    QString meta = entry->machine + QStringLiteral("  /  ") + s.cmd + QStringLiteral("  /  ") + projectContext(s);
    m_fullMeta = projectContext(s);
    m_providerBadge->setValue(s.cmd); m_machineBadge->setValue(entry->machine);
    meta += '\n' + (s.cwd.isEmpty() ? s.name : s.cwd);
    if (s.state == "archived" && s.archivedAt > 0) meta += '\n' + tr("Archived %1").arg(QDateTime::fromSecsSinceEpoch(qint64(s.archivedAt)).toLocalTime().toString("d MMM yyyy HH:mm:ss"));
    if (s.state == "archived") {
        const auto reason = m_details.value("completion_reason").toString();
        if (reason == "clean_exit") meta += '\n' + tr("Session closed normally");
        else if (reason == "user_archived") meta += '\n' + tr("Moved to archive");
    }
    m_meta->setToolTip(sessionTooltip({meta, m_details.value("model").toString(s.model)}, 420).html); updateHeaderText();
    const bool waiting=entry->online && SessionPresentation::waitingIdle(s);
    m_badge->setText(waiting?SessionPresentation::waitingCaption(s):state); const auto color = waiting
        ? QColor(m_dark ? "#edd096" : "#795913")
        : tone(s.state=="running" && s.processState!="exited" && entry->online && s.phase=="error"?QStringLiteral("Error"):state, m_dark);
    setWorkspaceStyle(m_badge, QString("color:%1; background:%2; border-radius:5px; padding:4px 8px; font-size:11px; font-weight:600;")
                          .arg(color.name(), m_dark ? "#2b323d" : "#edf1f5"));
    m_badge->setMinimumWidth(state == "Background" ? m_badge->fontMetrics().horizontalAdvance(state) + 16 : 0);
    s.model = m_details.value("model").toString(s.model); s.effort = m_details.value("effort").toString(s.effort);
    const QString modelText = s.model + (s.effort.isEmpty() ? QString() : "  " + s.effort);
    // Details render on every poll; elide again only when the text or font changes.
    if (const auto elision = modelText + '\n' + m_model->font().key(); elision != m_modelElision) {
        m_modelElision = elision; m_model->setText(m_model->fontMetrics().elidedText(modelText, Qt::ElideMiddle, 260)); m_model->setToolTip(modelText);
    }
    m_model->setAccessibleName(modelText); m_model->setVisible(!modelText.isEmpty());
    const bool archived = s.state == "archived";
    m_forkAction->setEnabled(!terminating && entry->online && !m_pending && m_forkKey.isEmpty() && !m_settingsRequest && !m_composer->isSending(m_selectedKey)
        && m_details.value("fork_supported").toBool());
    m_forkAction->setToolTip(m_details.value("fork_reason").toString(tr("Create an independent conversation with the same context and working folder")));
    const bool answering = std::any_of(m_pendingAnswers.cbegin(), m_pendingAnswers.cend(), [this](const PendingAnswer &p) { return p.key == m_selectedKey; });
    const bool updating = terminating || m_pending || m_settingsRequest || !m_renameKey.isEmpty() || m_composer->isSending(m_selectedKey) || answering;
    const bool occupied = archived && archiveNameOccupied(*entry);
    if (m_pause->property("archiveRestore").toBool() != archived) {
        m_pause->setProperty("archiveRestore", archived); m_pause->style()->unpolish(m_pause); m_pause->style()->polish(m_pause);
    }
    const bool terminalAvailable=NativeSessionCapabilities::terminal(s,m_details);
    m_open->setVisible(!archived && (s.cmd=="dsh"||terminalAvailable));
    const QString openCaption = s.cmd == "dsh" ? tr("Open native DeepSeek UI in browser") : m_clipboard ? tr("Copy connect command") : tr("Open externally");
    m_open->setToolTip(openCaption); m_open->setAccessibleName(openCaption); m_open->setText({});
    m_open->setIcon(workspaceIcon(s.cmd != "dsh" && m_clipboard ? "copy" : "external", QColor(m_muted))); m_open->setEnabled(entry->online && !updating && !archived && (s.cmd=="dsh"||terminalAvailable));
    const QString pauseCaption = updating ? tr("Updating…") : archived ? tr("Restore session") : s.state == "running" ? tr("Pause session") : tr("Resume session");
    m_pause->setAccessibleName(pauseCaption); m_pause->setText({});
    m_pause->setIcon(workspaceIcon(s.state == "running" ? "pause" : "play", QColor(archived ? m_accent : m_muted)));
    // Codex defers SessionStart after resume; its verified restored conversation can pause too.
    const bool canPause = s.state == "running" && s.resumable && (s.activity == "idle" || m_details.value("auth_required").toBool()) && s.processState != "exited"
        && (m_details.value("expected_id").toString().isEmpty() || m_details.value("resume_message_can_send").toBool());
    m_pause->setEnabled(entry->online && !updating && (archived ? (s.resumable && !s.archiveId.isEmpty() && !occupied) :
        ((canPause && NativeSessionCapabilities::action(s,"pause",m_details)) || (s.state != "running" && s.resumable && NativeSessionCapabilities::action(s,"resume",m_details)))));
    m_more->setEnabled(entry->online && !updating && (!archived || !s.archiveId.isEmpty()));
    m_renameAction->setEnabled(NativeSessionCapabilities::action(s,"rename",m_details) && entry->online && !updating && !s.name.section('/', 0, 0).isEmpty() && !s.name.section('/', 1, 1).isEmpty() && (!archived || !s.archiveId.isEmpty()));
    m_archiveAction->setVisible(s.cmd != "dsh" && (s.state == "paused" || s.state == "stopped"));
    m_archiveAction->setEnabled(!updating && NativeSessionCapabilities::action(s,"archive",m_details));
    m_forgetAction->setEnabled(!updating && NativeSessionCapabilities::action(s,s.state=="running"?"terminate":"forget",m_details));
    m_forgetAction->setToolTip(s.cmd=="opencode" && s.state=="running" ? tr("Pause OpenCode before forgetting its saved session.") : QString());
    m_forgetAction->setText(archived ? tr("Forget archived session…") : s.state == "running" ? tr("Terminate session…") : tr("Forget saved session…"));
    const auto questionError=m_details.value("pending_questions_error").toString();
    QString hint;
    if (terminating) hint = tr("Waiting for the session to stop. Its native conversation history is kept.");
    else if (!entry->online) hint = tr("Machine unavailable. Showing the last known state; actions will return when it reconnects.");
    else if (archived && !m_details.value("last_restore_error").toString().isEmpty())
        hint = tr("Last restore failed: %1").arg(m_details.value("last_restore_error").toString());
    else if (!m_inspectError.isEmpty()) hint = m_inspectError;
    else if (!questionError.isEmpty()) hint = tr("Could not load agent questions: %1. Check Terminal.").arg(questionError);
    else if (archived && occupied) hint = tr("Another active or saved session uses this name. Restore becomes available when that name is free.");
    else if (archived && !s.resumable) hint = tr("This session is archived. Its conversation history is currently unavailable, so it cannot be restored.");
    else if (archived) hint = tr("This session was archived. Restore explicitly to continue the same conversation; Resume all never starts archived sessions.");
    else if (!s.tracked && s.activity.isEmpty()) hint = tr("Activity is not tracked for this session. Sessions started with HGS 1.9 report their tools and status here.");
    else if (s.state == "paused") hint = tr("Conversation saved. Resume to continue with the same context.");
    else if (s.state == "stopped") hint = tr("Agent stopped. Its saved conversation can be resumed.");
    else if (s.processState == "exited") hint = tr("The agent process exited, but its terminal session still exists. Open the terminal to inspect it.");
    else if (s.conversationState == "ended" && s.activity != "idle") hint = tr("The last conversation ended, but the agent process is still running. Its current activity is unconfirmed; open the terminal to check.");
    else if (state == "Needs approval" || state == "Needs input") hint = tr("Your agent needs you. Open its terminal to respond.");
    else if (s.activity == "busy") hint = tr("Agent is active. Pause becomes available after the response and subagents finish.");
    else if (SessionPresentation::backgroundIdle(s)) hint = (s.backgroundProcesses == 1
        ? tr("The agent has replied, but a command is still running.") : tr("The agent has replied, but %1 commands are still running.").arg(s.backgroundProcesses))
        + QStringLiteral(" ") + (ProcessSettings::enabled() ? tr("Open Processes to follow their output or stop them.") : tr("Enable Settings > Processes to inspect them."))
        + QStringLiteral(" ") + tr("You can send another message.");
    else if (s.activity != "idle") hint = tr("Waiting for the agent to confirm its state. Open its terminal if it needs attention.");
    else hint = tr("Ready for your next message. Finishing a response does not mark the task complete.");
    m_hint->setText(hint);
    const auto goal = m_details.contains("goal") ? m_details.value("goal").toObject() : s.goal;
    const bool hasGoal = !goal.value("id").toString().isEmpty();
    m_goal->setVisible(hasGoal);
    const auto goalState = goal.value("status").toString();
    const QString goalColor = goalState == "active" ? (m_dark ? "#c4a6ed" : "#7650a5") :
        (goalState == "blocked" || goalState.endsWith("_limited")) ? (m_dark ? "#efc978" : "#92641b") :
        goalState == "complete" ? m_accent : m_muted;
    if (hasGoal) {
        m_goal->setText(tr("%1    %2    Details →").arg(SessionGoal::status(goal), SessionGoal::elapsed(goal)));
        m_goal->setAccessibleName(tr("%1: %2").arg(SessionGoal::status(goal), goal.value("objective").toString()));
        m_goal->setToolTip(sessionTooltip({goal.value("objective").toString(), SessionGoal::tokens(goal), tr("Goal state is independent of the current response.")}, 380).html);
        setWorkspaceStyle(m_goal, QString("QPushButton { text-align:left; color:%1; background:transparent; border:0; border-left:3px solid %1; border-radius:0; padding:7px 10px; } QPushButton:hover { background:%2; }").arg(goalColor, m_dark ? "#29323d" : "#e9eef4"));
    }
    m_badge->setToolTip(sessionTooltip({hint, m_model->text()}, 380).html);
    m_pause->setToolTip(sessionTooltip({pauseCaption, !NativeSessionCapabilities::action(s,archived?"restore":s.state=="running"?"pause":"resume",m_details) ? NativeSessionCapabilities::actionReason(s,archived?"restore":s.state=="running"?"pause":"resume",m_details) : canPause
        ? tr("Close the idle agent and save its conversation. Resume starts it again with the same history.") : hint}, 380).html);
    const bool contextVisible = !entry->online || !m_inspectError.isEmpty() || !questionError.isEmpty()
        || (archived && (occupied || !m_details.value("last_restore_error").toString().isEmpty()));
    const bool contextError = !entry->online || !m_inspectError.isEmpty() || !questionError.isEmpty()
        || (archived && !m_details.value("last_restore_error").toString().isEmpty());
    m_hint->setProperty("active", contextVisible); m_hint->setProperty("error", contextError); m_hint->hide();
    m_nativeSignIn->setVisible(s.cmd == "dsh" && entry->online && m_details.value("auth_required").toBool() && s.runtimeState == "live");
    setWorkspaceStyle(m_nativeSignIn, QString("QWidget#nativeSignInBanner { background:%1; border:1px solid %2; border-radius:8px; } QLabel { color:%3; background:transparent; border:0; }")
        .arg(m_dark ? "#40371e" : "#fff0ba", m_dark ? "#927140" : "#c6a15f", m_dark ? "#ffda76" : "#392900"));
    auto activityDetails = m_details;
    if (!entry->online || !m_inspectError.isEmpty() || s.state != "running") activityDetails["runtime_state"] = "unavailable";
    activityDetails["queue_sending"] = m_queueSendRequest && m_queueSendKey == m_selectedKey;
    if (!entry->online || !m_inspectError.isEmpty()) { auto queue=activityDetails["input_queue"].toObject(); queue["can_send_now"]=false; activityDetails["input_queue"]=queue; }
    m_activityView->setTimeline(activityDetails, m_events, m_localMessages.value(m_selectedKey), s.prompt, s.tracked);
    m_composer->setAttachmentAvailability(s.cmd!="opencode" && m_details.value("attachments_supported").toBool(true),
        m_details.value("attachments_reason").toString(tr("This native adapter does not support file attachments. Your existing files remain saved; remove them to send text.")));
    const QString messageProblem = messageBlockReason(*entry);
    m_composer->setSessionStatus(contextVisible ? hint : QString(), contextError,
        contextError && !messageProblem.isEmpty());
    if (!messageProblem.isEmpty() && canStartStopped(*entry))
        m_composer->setAvailability(true, tr("Session stopped. Send asks whether to resume it or start fresh."));
    else m_composer->setAvailability(messageProblem.isEmpty(), messageProblem);
    const auto phase=m_details.value("phase").toString(entry->session.phase);
    const bool working=QStringList{"working","tool","compacting"}.contains(phase)&&entry->session.state=="running";
    restoreInterruptedPrompt();
    // Claude's suggested next message lasts while the session waits for one.
    const auto suggestion = m_details.value("prompt_suggestion").toObject();
    m_composer->setSuggestion(m_selectedKey, phase == "idle" && entry->online && s.state == "running" ? suggestion.value("text").toString() : QString(),
        suggestion.value("at").toDouble());
    m_composer->setInterruptAvailability(working,entry->online&&m_details.value("interrupt_supported").toBool()&&!m_interruptRequest,
        m_interruptRequest?tr("Interrupt is being sent…"):!entry->online?tr("Machine is offline"):
        !m_details.value("interrupt_supported").toBool()?tr("Interrupt is unavailable. Open Terminal to stop this turn."):QString());
    const QString settingsProblem = modelSettingsBlockReason(*entry);
    m_composer->setSettingsTerminalAvailable(s.cmd == "claude" && entry->online && s.state == "running");
    m_composer->setModelSettings(m_details.value("settings_model").toString(s.model), s.effort, m_details.value("model_options").toArray(), settingsProblem.isEmpty(),
        settingsProblem.isEmpty() ? m_details.value("settings_change_reason").toString() : settingsProblem,
        m_details.value("pending_model").toString(), m_details.value("pending_effort").toString(),
        m_details.value("settings_apply_when").toString());
    m_composer->setSettingsNotice(m_details.value("settings_change_notice").toString());
    renderQuestion(*entry);
    updateSessionFeedback();
    m_terminal->setSession(m_client.executable(), entry->host, s.name, m_details.value("run_id").toString(), entry->machine);
    m_nativeUi->setSession(entry->host, s.cmd == "dsh" ? s.name : QString(), m_details.value("run_id").toString(s.runId), entry->machine, s.cmd == "dsh" && entry->online && !archived);
    const bool terminalTab = terminalAvailable && m_subagentId.isEmpty();
    // Loads the optional plugin on the first DeepSeek session, not at startup.
    const bool nativeTab = s.cmd == "dsh" && m_subagentId.isEmpty() && NativeBrowserPlugin::factory();
    if (m_detailTabs->isTabVisible(1) != terminalTab) m_detailTabs->setTabVisible(1, terminalTab);
    if (m_detailTabs->isTabVisible(2) != nativeTab) m_detailTabs->setTabVisible(2, nativeTab);
    if (!terminalTab && m_detailTabs->currentIndex() == 1) m_detailTabs->setCurrentIndex(0);
    if (!nativeTab && m_detailTabs->currentIndex() == 2) m_detailTabs->setCurrentIndex(0);
    m_terminal->setAvailable(entry->online && s.state == "running" && terminalAvailable,
        !entry->online ? tr("Machine unavailable. Reconnect when it is online.") :
        !terminalAvailable ? NativeSessionCapabilities::terminalReason(s,m_details) : tr("Resume this session before opening its terminal."));
    // A compact roster keeps many agents visible at once. A finished child
    // turn is Ready, never proof that its overall task or process has ended.
    const QString source = m_details.value("subagent_source").toString(s.subagentSource);
    s.subagentSource = source;
    s.subagentCountsComplete = m_details.value("subagent_counts_complete").toBool(s.subagentCountsComplete);
    s.subagentActiveCount = m_details.value("subagent_active_count").toInt(s.subagentActiveCount);
    s.subagentCount = s.subagentActiveCount;
    s.subagentTotalCount = m_details.value("subagent_total_count").toInt(s.subagentTotalCount);
    const auto agents = m_details.value("subagents").toObject();
    QList<QJsonObject> roster;
    const auto groups = m_details.value("subagent_groups").toObject();
    if (source == "hook_profiles" || source == "mixed_hooks") for (auto it = groups.begin(); it != groups.end(); ++it) {
        auto group = it.value().toObject(); group["id"] = "profile:" + it.key(); group["group"] = true;
        if (!group.contains("label")) group["label"] = it.key();
        roster.append(group);
    }
    for (auto it = agents.begin(); source != "hook_profiles" && it != agents.end(); ++it) {
        auto child = it.value().toObject(); child["id"] = it.key(); roster.append(child);
    }
    if (roster.isEmpty()) for (const auto &value : m_details.value("subagent_previews").toArray(s.subagentPreviews)) roster.append(value.toObject());
    const auto retainedRosterSize = roster.size();
    std::stable_sort(roster.begin(), roster.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const bool activeA = childState(a) == "working" || a.value("active_count").toInt() > 0;
        const bool activeB = childState(b) == "working" || b.value("active_count").toInt() > 0;
        if (activeA != activeB) return activeA;
        return a.value("updated").toDouble() > b.value("updated").toDouble();
    });
    const int rosterWidth = qMax(200, m_children->viewport()->width() - 24);
    const int stateWidth = 58, nameWidth = qMax(110, rosterWidth - stateWidth - 12);
    QFont rosterFont = m_children->font(); rosterFont.setPixelSize(13); const QFontMetrics metrics(rosterFont);
    const auto elide = [&](const QString &text, int width) { return htmlText(metrics.elidedText(text.simplified(), Qt::ElideRight, width)); };
    const bool currentChildren = currentActivity(s, entry->online);
    QString children = QString("<style>body{color:%1;font-size:12px;}p{margin:5px 0 9px;}td{padding:7px 0;}h3{font-size:12px;margin:16px 0 8px;}</style>").arg(m_fg);
    children += QString("<p><b>%1</b> <span style='color:%2'>%3</span></p><p style='color:%2'>%4</p>")
        .arg(tr("Main agent"), m_muted, htmlText(state), htmlText(currentAction(s, entry->online)));
    if (hasGoal) {
        children += QString("<h3>%1</h3><p style='color:%2'><b>%3</b></p><p>%4</p><p style='color:%5'>%6</p>")
            .arg(tr("Goal"), goalColor, htmlText(SessionGoal::status(goal)), htmlText(goal.value("objective").toString()),
                m_muted, htmlText(SessionGoal::metrics(goal).join('\n')).replace('\n', "<br>"));
        if (!goal.value("completion_criterion").toString().isEmpty()) children += "<p>" + htmlText(goal.value("completion_criterion").toString()) + "</p>";
        if (!goal.value("reason").toString().isEmpty()) children += "<p>" + htmlText(goal.value("reason").toString()) + "</p>";
        if (!entry->online || s.state != "running" || s.processState == "exited")
            children += QString("<p style='color:%1'>%2</p>").arg(m_muted, tr("The agent is not connected here. Goal progress is not confirmed."));
        children += QString("<p style='font-size:11px;color:%1'>%2</p>").arg(m_muted, tr("Reported by the agent. A finished response does not complete this goal."));
    } else if (m_details.value("goal_source") == "unavailable") {
        children += QString("<h3>%1</h3><p style='color:%2'>%3</p>").arg(tr("Goal"), m_muted, tr("Goal data is unavailable for this conversation or agent version."));
    }
    const auto taskLists = m_details.value("task_lists").toObject();
    children += QString("<h3>%1</h3>").arg(tr("Tasks"));
    if (taskLists.isEmpty()) children += QString("<p style='color:%1'>%2</p>").arg(m_muted, tr("No task list reported yet. Tasks appear when the agent updates its plan."));
    QStringList taskOwners = taskLists.keys(); taskOwners.removeAll("main");
    if (taskLists.contains("main")) taskOwners.prepend("main");
    for (const auto &owner : taskOwners) {
        const auto list = taskLists.value(owner).toObject(); const auto items = list.value("items").toArray();
        QString ownerLabel = list.value("source").toString().startsWith("Task") ? tr("Session tasks") : tr("Main agent");
        if (owner.startsWith("agent:")) {
            const QString id = owner.mid(6); const auto agent = agents.value(id).toObject();
            ownerLabel = agent.value("label").toString(agent.value("name").toString(id));
        }
        int done = 0; for (const auto &value : items) if (value.toObject().value("status").toString() == "completed") ++done;
        const bool recorded = !currentChildren || list.value("run_id").toString() != m_details.value("run_id").toString();
        children += QString("<p><b>%1</b><br><span style='color:%2'>%3%4</span></p>").arg(htmlText(ownerLabel), m_muted,
            tr("%1 of %2 completed").arg(done).arg(items.size()), recorded ? tr(" (Last recorded)") : QString());
        if (items.isEmpty()) children += QString("<p style='color:%1'>%2</p>").arg(m_muted, tr("The agent cleared this list."));
        for (const auto &value : items) {
            const auto task = value.toObject(); const QString taskState = task.value("status").toString();
            const QString marker = taskState == "completed" ? QStringLiteral("✓") : taskState == "in_progress" ? QStringLiteral("◉") : QStringLiteral("○");
            const QString stateLabel = taskState == "completed" ? tr("Done") : taskState == "in_progress" ? tr("In progress") : taskState == "pending" ? tr("Planned") : tr("Unknown");
            const QString color = taskState == "in_progress" && !recorded ? m_accent : m_muted;
            children += QString("<p style='margin:7px 0 10px;'><span style='color:%1'>%2</span> %3<br><span style='font-size:11px;color:%1'>%4%5</span></p>")
                .arg(color, marker, htmlText(task.value("title").toString()), stateLabel,
                    task.value("owner").toString().isEmpty() ? QString() : " / " + htmlText(task.value("owner").toString()));
        }
        if (list.value("truncated").toBool()) children += QString("<p style='color:%1'>%2</p>").arg(m_muted, tr("Showing the first 100 tasks."));
    }
    children += QString("<h3>%1 <span style='color:%2;font-weight:normal'>%3</span></h3>")
        .arg(source == "hook_profiles" ? tr("Profile groups") : tr("Subagents"), m_muted,
            currentChildren ? childCount(s, true) : tr("Last recorded"));
    if (source == "hook_profiles") children += QString("<p style='color:%1'>%2</p>").arg(m_muted, tr("Observed runs grouped by profile; individual agent identities are not reported."));
    if (roster.isEmpty()) {
        const bool unavailable = source == "unavailable" || !s.tracked;
        const QString emptyHint = unavailable ? tr("Subagent activity is unavailable for this session.") :
            source == "hook_profiles" && !s.subagentCountsComplete ? tr("Exact subagent counts are unavailable for this run.") :
            tr("No subagent activity has been reported yet.");
        children += QString("<p style='color:%1'>%2</p>").arg(m_muted, emptyHint);
    } else {
        children += "<table width='100%' cellspacing='0' cellpadding='0'>";
        for (const auto &child : roster) {
            QString name = child.value("label").toString();
            if (name.isEmpty()) name = child.value("name").toString();
            if (name.isEmpty()) name = child.value("id").toString();
            if (name.isEmpty()) name = tr("Unnamed agent");
            const QString childState = SessionPresentation::childState(child);
            QString stateText = childStatus(child);
            if (child.value("group").toBool()) {
                if (s.subagentCountsComplete && child.value("active_count").isDouble() && child.value("total_count").isDouble())
                    stateText = QString("%1/%2").arg(child.value("active_count").toInt()).arg(child.value("total_count").toInt());
                else stateText = tr("Unknown");
            }
            QString detail = child.value("detail").toString();
            if (detail.isEmpty()) detail = child.value("task").toString(child.value("description").toString());
            const auto tool = child.value("current_tool").toString();
            if (!tool.isEmpty()) detail = tool + (detail.isEmpty() ? QString() : QStringLiteral(" / ") + detail);
            if (detail.isEmpty()) detail = tr("No action reported");
            if (!currentChildren) stateText = tr("Recorded");
            const QColor childColor = tone(currentChildren && childState == "working" ? "Working" : stateText, m_dark);
            QString title = elide(name, nameWidth - 28);
            const auto id = child.value("id").toString();
            if (!child.value("group").toBool() && agents.contains(id))
                title = QString("<a href='hgs-agent:%1' style='color:%2;text-decoration:none'>%3 →</a>")
                    .arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), m_fg, title);
            children += QString("<tr><td width='%1'><font color='%2'>●</font> <b>%3</b><br><span style='color:%4;font-size:11px'>%5</span></td><td valign='top' align='right' width='%6' style='color:%2;font-size:11px'>%7</td></tr>")
                .arg(nameWidth).arg(childColor.name(), title, m_muted, elide(detail, nameWidth)).arg(stateWidth).arg(htmlText(stateText));
        }
        children += "</table>";
        if (agents.isEmpty() && s.subagentTotalCount > retainedRosterSize) children += QString("<p style='color:%1'>%2</p>").arg(m_muted, tr("%1 recent records available").arg(retainedRosterSize));
    }
    if (!m_subagentId.isEmpty()) {
        const auto child = childRoster(*entry).value(m_subagentId).toObject();
        children = QString("<h3>%1</h3><p>%2</p><p>%3</p><h3>%4</h3>")
            .arg(htmlText(childName(child, m_subagentId)), htmlText(childStatus(child)),
                htmlText(child.value("task").toString(child.value("detail").toString())), tr("Tasks"));
        const auto tasks = taskLists.value("agent:" + m_subagentId).toObject().value("items").toArray();
        if (tasks.isEmpty()) children += "<p>" + tr("No task list reported by this agent.") + "</p>";
        for (const auto &item : tasks) {
            const auto task = item.toObject();
            children += QString("<p>%1 %2</p>").arg(task.value("status") == "completed" ? "✓" : "○", htmlText(task.value("title").toString()));
        }
        m_providerBadge->setValue(m_subagentDetails.value("provider").toString(child.value("provider").toString(s.cmd)));
        m_fullTitle = (project ? project->name + " / " + sessionLabel(s) : displayTitle(s)) + " / " + childName(child, m_subagentId); updateHeaderText();
        m_badge->setText(childStatus(child)); m_badge->setToolTip({});
        m_subagentComposer->setAvailability(!isTerminating(*entry) && entry->online && m_subagentDetails.value("send_supported").toBool(), entry->online ? QString() : tr("Machine unavailable. Your draft stays here."));
        m_goal->hide(); m_model->hide(); m_open->hide(); m_pause->hide(); m_more->hide();
        m_forkAction->setEnabled(false); m_renameAction->setEnabled(false);
    } else { m_pause->show(); m_more->show(); }
    updateBrowser(m_children, children, m_childrenHtml);
    const auto conversation = m_details.value("conversation_id").toString(s.conversationId);
    const auto run = m_details.value("run_id").toString(s.runId);
    QString identity = conversation.isEmpty() ? run : conversation;
    QString identityTitle = conversation.isEmpty() && !run.isEmpty() ? tr("Run ID") : tr("Session ID");
    QString copyCaption = conversation.isEmpty() && !run.isEmpty() ? tr("Copy run ID") : tr("Copy session ID");
    if (!m_subagentId.isEmpty()) {
        const auto childConversation = m_subagentDetails.value("conversation_id").toString();
        identity = childConversation.isEmpty() ? m_subagentId : childConversation;
        identityTitle = childConversation.isEmpty() ? tr("Agent ID") : tr("Session ID");
        copyCaption = childConversation.isEmpty() ? tr("Copy agent ID") : tr("Copy session ID");
    }
    m_detailsIdTitle->setText(identityTitle);
    const auto identityText = identity.isEmpty() ? tr("Not confirmed") : identity;
    if (m_detailsIdValue->text() != identityText) m_detailsIdValue->setText(identityText);
    m_detailsIdValue->setProperty("copyValue", identity); m_detailsIdValue->setAccessibleName(identityTitle + ": " + identityText);
    m_copySessionId->setEnabled(!identity.isEmpty());
    m_copySessionId->setToolTip(copyCaption); m_copySessionId->setAccessibleName(copyCaption);
    QString info = QString("<h3>Session</h3><p>%1</p><h3>Directory</h3><p>%2</p><h3>Last recorded event</h3><p>%3</p><h3>History</h3><p>Activity stays on %4. Closing this window does not stop your agent.</p>")
        .arg(htmlText(s.name),
             htmlText(s.cwd.isEmpty() ? tr("Not reported") : s.cwd),
             m_details.value("last_event_at").toDouble() ? QDateTime::fromSecsSinceEpoch(qint64(m_details.value("last_event_at").toDouble())).toLocalTime().toString("ddd, d MMM HH:mm:ss") : tr("No events yet"), htmlText(entry->machine));
    if (!m_model->text().isEmpty()) info += QString("<h3>Model</h3><p>%1</p>").arg(htmlText(m_model->text()));
    if (!s.gitRoot.isEmpty()) info += QString("<h3>Git</h3><p>%1<br>%2</p>").arg(htmlText(projectContext(s)), htmlText(s.gitRoot));
    if (!m_subagentId.isEmpty()) info = QString("<h3>%1</h3><p>%2</p><h3>%3</h3><p>%4</p><h3>%5</h3><p>%6</p>")
        .arg(tr("Subagent"), htmlText(m_subagentId), tr("Parent session"), htmlText(s.name), tr("Directory"), htmlText(m_subagentCwd));
    info += SessionUsage::html((m_subagentId.isEmpty() ? m_details : m_subagentDetails).value("session_usage").toObject(), m_muted, !m_subagentId.isEmpty());
    const QStringList infoContext{entry->key, m_details.value("conversation_id").toString(s.conversationId), m_subagentId};
    auto previousInfo = m_info->property("inspectorHtml").toString();
    updateBrowser(m_info, info, previousInfo, m_info->property("context").toStringList() == infoContext);
    m_info->setProperty("inspectorHtml", previousInfo); m_info->setProperty("context", infoContext);
}

QString SessionsWindow::modelSettingsBlockReason(const Entry &entry) const
{
    if (!entry.online) return tr("Machine unavailable. Reconnect to change model settings.");
    if (m_settingsRequest) return tr("Updating model settings…");
    if (isTerminating(entry) || m_pending || !m_renameKey.isEmpty() || !m_forkKey.isEmpty() || m_composer->isSending(entry.key))
        return tr("Wait for the session update to finish.");
    for (const auto &answer : m_pendingAnswers) if (answer.key == entry.key) return tr("Wait for the answer to be delivered.");
    if (!m_inspectError.isEmpty()) return tr("Refresh the session before changing model settings.");
    if (!m_details.value("settings_change_supported").toBool()) {
        const QString reason = m_details.value("settings_change_reason").toString();
        return reason.isEmpty() ? tr("Model settings are not available for this session.") : reason;
    }
    return {};
}

void SessionsWindow::changeModelSettings(const QString &key, const QString &model, const QString &effort)
{
    const auto *entry = selected();
    if (!entry || entry->key != key || !modelSettingsBlockReason(*entry).isEmpty()) { renderDetails(); return; }
    m_settingsKey = key; m_settingsRun = m_details.value("run_id").toString();
    m_settingsConversation = m_details.value("conversation_id").toString();
    m_settingsHost = entry->host; m_settingsName = entry->session.name; m_settingsAutomatic = false;
    m_settingsRequest = m_client.requestSettings(entry->host, entry->session.name, model, effort, m_settingsRun, m_settingsConversation);
    renderDetails();
}

QByteArray SessionsWindow::settingsIdentity(const QString &key, const PendingSettings &pending)
{
    return QJsonDocument(QJsonArray{key, pending.run, pending.conversation, pending.id, pending.model, pending.effort}).toJson(QJsonDocument::Compact);
}

// The listing's row for a queued change's run and conversation; null without a
// current listing or after either changed.
const SessionInfo *SessionsWindow::listedSettingsSession(const PendingSettings &pending) const
{
    const auto *box = pending.host.isEmpty() ? &m_fleet.local() : m_fleet.peer(pending.host);
    if (!box || !box->ok || (pending.host.isEmpty() && !m_connectionError.isEmpty())) return nullptr;
    for (const auto &session : box->sessions)
        if (session.name == pending.name && session.runId == pending.run
            && (session.conversationId.isEmpty() || session.conversationId == pending.conversation)) return &session;
    return nullptr;
}

static bool listedReady(const SessionInfo &session)
{
    return session.state == "running" && session.phase == "idle" && session.activity == "idle" && session.processState == "running";
}

// Each pending change gets one automatic attempt. One that dispatch scheduled
// gets one more per Ready transition: after the session left Ready, once the
// listing and, for the selected session, inspect both show it Ready again.
// Never turn a failed native change into a polling loop.
bool SessionsWindow::settingsAttemptAllowed(const QString &key, const PendingSettings &pending, const QByteArray &identity) const
{
    if (!m_settingsAttempts.contains(identity)) return true;
    const auto scheduled = m_scheduledSettings.constFind(key);
    if (scheduled == m_scheduledSettings.cend() || scheduled->identity != identity || !scheduled->rearmed) return false;
    const auto *session = listedSettingsSession(pending);
    if (!session || !listedReady(*session)) return false;
    return key != m_selectedKey || (m_details.value("run_id").toString() == pending.run
        && m_details.value("conversation_id").toString() == pending.conversation && m_details.value("settings_apply_when") == "now");
}

void SessionsWindow::requestAutomaticSettings(const QString &key, const PendingSettings &pending, const QByteArray &identity)
{
    m_settingsKey = key; m_settingsHost = pending.host; m_settingsName = pending.name;
    m_settingsRun = pending.run; m_settingsConversation = pending.conversation; m_settingsAutomatic = true;
    m_settingsAttempts.insert(identity); m_settingsIdentity = identity; m_scheduledSettings.remove(key);
    m_settingsRequest = m_client.requestSettings(pending.host, pending.name, pending.model, pending.effort, pending.run, pending.conversation, pending.id);
    renderDetails();
}

void SessionsWindow::applyPendingModelSettings()
{
    const auto *entry = selected();
    if (!entry) return;
    const QString model = m_details.value("pending_model").toString(), effort = m_details.value("pending_effort").toString();
    if (model.isEmpty() && effort.isEmpty()) { m_queuedSettings.remove(entry->key); m_scheduledSettings.remove(entry->key); return; }
    const QString pendingId = m_details.value("pending_settings_id").toString();
    if (pendingId.isEmpty()) return;
    const QString key = entry->key;
    const PendingSettings pending{entry->host, entry->session.name, m_details.value("run_id").toString(),
        m_details.value("conversation_id").toString(), model, effort, pendingId};
    m_queuedSettings[key] = pending;
    const auto identity = settingsIdentity(key, pending);
    if (m_details.value("settings_apply_when") != "now") {
        // Inspect shows the session not Ready, as while a chooser waits.
        const auto scheduled = m_scheduledSettings.find(key);
        if (scheduled != m_scheduledSettings.end() && scheduled->identity == identity) scheduled->rearmed = true;
        return;
    }
    if (modelSettingsBlockReason(*entry).isEmpty() && settingsAttemptAllowed(key, pending, identity))
        requestAutomaticSettings(key, pending, identity);
}

void SessionsWindow::applyQueuedModelSettings()
{
    // The listing shows whether a session whose automatic apply was scheduled
    // left Ready. A change dropped or replaced meanwhile needs no other attempt.
    for (auto it = m_scheduledSettings.begin(); it != m_scheduledSettings.end();) {
        const auto queued = m_queuedSettings.constFind(it.key());
        if (queued == m_queuedSettings.cend() || settingsIdentity(it.key(), *queued) != it->identity) { it = m_scheduledSettings.erase(it); continue; }
        if (const auto *session = listedSettingsSession(*queued); session && !listedReady(*session)) it->rearmed = true;
        ++it;
    }
    if (m_settingsRequest || m_pending || !m_renameKey.isEmpty() || !m_forkKey.isEmpty()) return;
    for (auto it = m_queuedSettings.cbegin(); it != m_queuedSettings.cend(); ++it) {
        const QString key = it.key(); const auto pending = it.value();
        if (pending.id.isEmpty() || m_terminating.contains(key) || m_composer->isSending(key)) continue;
        bool answering = false;
        for (const auto &answer : m_pendingAnswers) answering |= answer.key == key;
        if (answering) continue;
        const auto *session = listedSettingsSession(pending);
        if (!session || !listedReady(*session)) continue;
        const auto identity = settingsIdentity(key, pending);
        if (!settingsAttemptAllowed(key, pending, identity)) continue;
        requestAutomaticSettings(key, pending, identity); return;
    }
}

QString SessionsWindow::messageBlockReason(const Entry &entry) const
{
    if (entry.key == m_startSend.key) return tr("Waiting for the session to start. This exact message will be sent automatically when ready. Cancel send to edit.");
    if (m_compacts.contains(entry.key)) return tr("Waiting for compaction. This exact message will be sent automatically after it completes. Cancel send to edit.");
    if (entry.key == m_clearKey && m_clearRequest) return tr("Wait for the context reset to finish. Your draft stays here.");
    if (isTerminating(entry)) return tr("The session is terminating. Your draft stays here.");
    if (entry.key == m_settingsKey && m_settingsRequest) return tr("Wait for the model settings change to finish.");
    if (!entry.online) return tr("Machine unavailable. Your draft stays here.");
    if (entry.session.state != "running") return tr("Resume the session to send a message.");
    if (m_pending || !m_renameKey.isEmpty()) return tr("Wait for the session update to finish.");
    if (!QStringList{"codex", "claude", "kimi", "dsh", "opencode"}.contains(entry.session.cmd)) return tr("Use the Terminal tab to type into this session.");
    if (!m_inspectError.isEmpty()) return tr("Refresh the session before sending.");
    if (m_details.isEmpty()) return tr("Checking the session…");
    if (!m_details.value("tracked").toBool()) return tr("This session has no HGS tracking. Use Terminal to send a message.");
    if (m_details.value("run_id").toString().isEmpty()) return tr("Waiting for the agent to finish starting. Open Terminal to check login or setup.");
    if (m_details.value("runtime_state").toString() != "live" || m_details.value("process_state").toString() != "running")
        return tr("The agent is not running. Resume it before sending.");
    if (entry.session.cmd == "dsh" && m_details.value("auth_required").toBool())
        return tr("Sign in to DeepSeek above. Your draft stays here.");
    if (!m_details.value("error").toString().isEmpty()) return entry.session.cmd=="opencode"?tr("Refresh the native session and check OpenCode configuration on this computer before sending."):tr("Check the agent in the Terminal tab before sending.");
    if (!m_details.value("expected_id").toString().isEmpty()) {
        if (m_details.value("resume_message_can_send").toBool()) return {};
        return m_details.value("resume_message_reason").toString(tr("Waiting for the restored conversation. Open Terminal to check startup."));
    }
    // Codex reports SessionStart only with its first turn. The CLI can verify a
    // fresh supervised run's native prompt before a conversation ID exists.
    if (m_details.value("conversation_id").toString().isEmpty()) {
        if (m_details.value("first_message_can_send").toBool()) return {};
        return m_details.value("first_message_reason").toString(tr("Finish login or setup in Terminal. Sending becomes available when the agent is ready."));
    }
    const QString phase = m_details.value("phase").toString();
    if (phase == "approval" || phase == "input") return entry.session.cmd=="opencode"?tr("Answer the pending native request in Activity before sending."):tr("Respond to the agent in the Terminal tab.");
    if (phase == "interrupted") {
        if (m_details.value("interrupted_message_can_send").toBool()) return {};
        return m_details.value("interrupted_message_reason").toString(tr("Waiting for the agent's input prompt after interruption. Open Terminal to check."));
    }
    if (phase == "error" && m_details.value("provider_error").isObject()) {
        if (m_details.value("error_message_can_send").toBool()) return {};
        return m_details.value("error_message_reason").toString(tr("Check the agent in Terminal before retrying."));
    }
    const QString activity = m_details.value("activity").toString();
    const bool ready = activity == "idle" && phase == "idle";
    const bool working = activity == "busy" && QStringList{"idle", "working", "tool", "compacting"}.contains(phase);
    if (!ready && !working) return tr("Waiting for the agent to confirm its input state.");
    return {};
}

void SessionsWindow::messageAction(const QString &id,const QString &action)
{
    const auto *entry=selected();if(!entry)return;
    auto &messages=m_localMessages[m_selectedKey];
    for(int i=0;i<messages.size();++i) {
        const auto message=messages[i].toObject();
        if(message.value("id").toString()!=id || message.value("status")!="error")continue;
        if(action=="delete"){messages.removeAt(i);renderDetails();return;}
        if(action=="inspect"){
            if(entry->session.cmd=="dsh")showNativeUi(*entry);
            else if(NativeSessionCapabilities::terminal(entry->session,m_details)){m_detailTabs->setCurrentWidget(m_terminal);m_terminal->connectSession();}
            else {inspect();showNotice(tr("Refreshing the native session. Check its provider configuration on %1 if the error remains.").arg(entry->machine));}
            return;
        }
        if(action!="retry" || message.value("uncertain").toBool() || m_composer->isSending(m_selectedKey))return;
        if(message.value("run_id")!=m_details.value("run_id") || message.value("conversation_id")!=m_details.value("conversation_id"))return;
        const auto reason=messageBlockReason(*entry);if(!reason.isEmpty()){showNotice(reason,true);return;}
        const QString key = m_selectedKey;
        if (m_retryReads.contains(key)) return;
        const Entry target = *entry;
        const QString run = message.value("run_id").toString();
        const QString conversation = message.value("conversation_id").toString();
        const auto cacheEvidence = sendEvidence(m_details);
        m_retryReads.insert(key);
        beginSendEvidence(key, cacheEvidence);
        AttachmentFiles::readAsync(message.value("attachments").toArray(), this,
            [this, key, target, message, id, run, conversation, cacheEvidence](const QList<MessageAttachment> &files, const QString &error) {
                m_retryReads.remove(key);
                const auto evidence = m_pendingSendEvidence.value(key, cacheEvidence);
                finishSendEvidence(key);
                if (!error.isEmpty()) { showNotice(error, true); return; }
                const auto current = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &value) { return value.key == key; });
                if (current == m_entries.cend() || current->host != target.host || current->session.name != target.session.name
                    || current->session.created != target.session.created || m_composer->isSending(key)
                    || !observedIdentityMatches(current->session, target.session, run, conversation)
                    || !current->online || isTerminating(*current)
                    || evidence.value("run_id").toString() != run || evidence.value("conversation_id").toString() != conversation
                    || evidence.value("runtime_state") != "live" || evidence.value("process_state") != "running"
                    || m_pending || !m_renameKey.isEmpty() || m_startSend.key == key || m_compacts.contains(key)
                    || (key == m_clearKey && m_clearRequest) || (key == m_settingsKey && m_settingsRequest)) return;
                bool retained = false;
                for (const auto &value : m_localMessages.value(key)) if (value.toObject() == message) { retained = true; break; }
                if (!retained) return;
                if (key == m_selectedKey) {
                    if (m_details.value("run_id").toString() != run || m_details.value("conversation_id").toString() != conversation) return;
                    sendMessage(key, message.value("text").toString(), files, id);
                } else {
                    // An accepted warm retry stays with its original session.
                    // A cold retry still needs the foreground consent dialog.
                    if (CacheStatus::expired(cacheEvidence) || CacheStatus::expired(evidence)) return;
                    deliverMessage(key, target.host, target.session.name, run, conversation,
                                   message.value("text").toString(), files, id, {}, false, evidence);
                }
            });
        return;
    }
}

void SessionsWindow::cancelCompactContinuation(const QString &key, const QString &reason)
{
    m_waitingPreparations.remove(key);
    if (!m_compacts.remove(key)) return;
    if (m_compacts.isEmpty() && m_startSend.key.isEmpty()) m_compactTimer.stop();
    m_composer->deliveryFinished(key,false,reason);
}

void SessionsWindow::compactToSend(const QString &key, const QString &host, const QString &name, const QString &run, const QString &conversation,
                                 const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId)
{
    if (m_compacts.contains(key) || !m_composer->draftMatches(key, text, attachments)) return;
    CompactContinuation pending;
    pending.key=key; pending.host=host; pending.name=name; pending.run=run; pending.conversation=conversation;
    pending.text=text; pending.attachments=attachments; pending.retryId=retryId;
    pending.started=QDateTime::currentMSecsSinceEpoch();
    m_compacts.insert(key,pending); m_compactTimer.start();
    const auto serial = ++m_waitingPreparationSerial; m_waitingPreparations.insert(key, serial);
    beginSendEvidence(key, key == m_selectedKey ? m_details : QJsonObject());
    m_composer->prepareWaitingToSendAsync(key,
        tr("Waiting for compaction. This exact message will be sent automatically after it completes. Cancel send to edit."),
        [this, key, host, name, run, conversation, serial](bool okay) {
            const auto evidence = key == m_selectedKey ? sendEvidence(m_details) : m_pendingSendEvidence.value(key);
            finishSendEvidence(key);
            if (m_waitingPreparations.value(key) != serial || !m_compacts.contains(key)) return;
            m_waitingPreparations.remove(key);
            const auto entry = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &value) { return value.key == key; });
            if (!okay || entry == m_entries.cend() || entry->host != host || entry->session.name != name
                || !entry->online || isTerminating(*entry) || m_pending || !m_renameKey.isEmpty()
                || (!evidence.isEmpty() && (evidence.value("run_id").toString() != run || evidence.value("conversation_id").toString() != conversation))
                || (key == m_selectedKey && (m_details.value("run_id").toString() != run || m_details.value("conversation_id").toString() != conversation))) {
                cancelCompactContinuation(key, tr("Compaction was cancelled. Your draft was kept without sending.")); return;
            }
            m_compacts[key].request = m_client.requestCompactContext(host, name, run, conversation);
            if (key == m_selectedKey) renderDetails();
        });
}

void SessionsWindow::pollCompactContinuations()
{
    if (!m_startSend.key.isEmpty()) {
        if (QDateTime::currentMSecsSinceEpoch()-m_startSend.started > 180000)
            cancelStartContinuation(tr("The agent did not become ready. Check Terminal; your draft was kept without sending."));
        else {
            const auto *box = m_startSend.host.isEmpty() ? &m_fleet.local() : m_fleet.peer(m_startSend.host);
            if (box && box->ok && (!m_startSend.host.isEmpty() || m_connectionError.isEmpty()))
                m_client.requestInspection(m_startSend.host,m_startSend.name,m_startSend.key == m_selectedKey ? m_cursor : 0,{},false);
        }
    }
    for (const auto &key : m_compacts.keys()) {
        const auto pending = m_compacts.value(key);
        if (QDateTime::currentMSecsSinceEpoch()-pending.started > 600000) {
            cancelCompactContinuation(key, tr("Compaction was not confirmed. Check Terminal; your draft was kept without sending."));
            continue;
        }
        if (pending.request || pending.commandId.isEmpty()) continue;
        const auto *box = pending.host.isEmpty() ? &m_fleet.local() : m_fleet.peer(pending.host);
        if (!box || !box->ok || (pending.host.isEmpty() && !m_connectionError.isEmpty())) continue;
        // This poll belongs to the accepted send, even when its row is hidden
        // by a filter, another page is open, or the workspace is closed.
        m_client.requestInspection(pending.host, pending.name, key == m_selectedKey ? m_cursor : 0, {}, false);
    }
}

void SessionsWindow::continueAfterCompact(const QString &host, const QString &name, const QJsonObject &data)
{
    for (const auto &key : m_compacts.keys()) {
        if (m_waitingPreparations.contains(key)) continue;
        const auto pending = m_compacts.value(key);
        if (pending.host != host || pending.name != name) continue;
        if (data.value("run_id") != pending.run || data.value("conversation_id") != pending.conversation) {
            cancelCompactContinuation(key, tr("Conversation changed. Your draft was kept without sending.")); return;
        }
        if (QDateTime::currentMSecsSinceEpoch()-pending.started > 600000) {
            cancelCompactContinuation(key, tr("Compaction was not confirmed. Check Terminal; your draft was kept without sending.")); return;
        }
        if (!m_composer->draftMatches(key,pending.text,pending.attachments)) {
            cancelCompactContinuation(key, tr("Draft changed. Send it when you are ready.")); return;
        }
        if (pending.request || pending.commandId.isEmpty()) return;
        const auto result=data.value("compact_context_request").toObject();
        if (result.value("request_id") != pending.commandId) return;
        const auto status=result.value("status").toString();
        if (status=="failed" || status=="cancelled" || status=="unchanged" || status=="uncertain") {
            cancelCompactContinuation(key, status=="unchanged" ? tr("No history could be compacted. Your draft was kept without sending.")
                : tr("Compaction did not complete. Check Terminal; your draft was kept without sending.")); return;
        }
        if (status!="completed" || data.value("phase")!="idle" || data.value("activity")!="idle"
            || data.value("runtime_state")!="live" || data.value("process_state")!="running"
            || !data.value("tracked").toBool() || !data.value("error").toString().isEmpty()
            || !data.value("expected_id").toString().isEmpty() || m_terminating.contains(key)) return;
        // Remove the intent before delivery: duplicate inspection signals must
        // never submit it twice. The CLI also verifies the compaction UUID.
        m_compacts.remove(key);
        if (m_compacts.isEmpty() && m_startSend.key.isEmpty()) m_compactTimer.stop();
        deliverMessage(key,host,name,pending.run,pending.conversation,pending.text,pending.attachments,pending.retryId,pending.commandId,true,data);
        return;
    }
}

void SessionsWindow::clearContext()
{
    const auto *entry=selected();
    // A Claude chooser hides the native composer: clearing first dismisses it.
    const bool dismiss=!m_details.value("clear_context_supported").toBool();
    if(!entry || !entry->online || m_clearRequest || m_composer->isSending(m_selectedKey)
        || (dismiss && !m_details.value("clear_context_dismiss").toBool()))return;
    if(findChild<QMessageBox *>("clearSessionConfirm"))return;
    const auto key=m_selectedKey,host=entry->host,name=entry->session.name;
    const auto run=m_details.value("run_id").toString(),conversation=m_details.value("conversation_id").toString();
    if(run.isEmpty() || conversation.isEmpty() || !m_inspectError.isEmpty())return;
    const QString text=dismiss
        ? tr("Claude in %1 on %2 is waiting for a choice in Terminal. Dismiss it and clear the conversation context?\n\n"
             "This presses Esc once, as in Terminal: a pending approval is denied and a question stays unanswered. "
             "Then the agent’s native clear command runs. Your unsent draft will be kept.")
        : tr("Clear the conversation context in %1 on %2?\n\nThis runs the agent’s native clear command. Future messages will start without the current conversation context. Your unsent draft will be kept.");
    QMessageBox dialog(QMessageBox::Warning,tr("Clear session?"),text.arg(name,entry->machine),QMessageBox::NoButton,this);
    dialog.setObjectName("clearSessionConfirm");dialog.setTextFormat(Qt::PlainText);
    auto *confirm=dialog.addButton(dismiss?tr("Dismiss and clear"):tr("Clear session"),QMessageBox::DestructiveRole);
    auto *cancel=dialog.addButton(QMessageBox::Cancel);dialog.setDefaultButton(cancel);dialog.setEscapeButton(cancel);
    dialog.exec();
    if(dialog.clickedButton()!=confirm)return;
    entry=selected();
    if(!entry || !entry->online || m_selectedKey!=key || entry->host!=host || entry->session.name!=name
        || m_details.value("run_id")!=run || m_details.value("conversation_id")!=conversation
        || m_clearRequest || m_composer->isSending(key) || !m_inspectError.isEmpty()
        || m_details.value("clear_context_supported").toBool()==dismiss
        || (dismiss && !m_details.value("clear_context_dismiss").toBool()))return;
    m_clearKey=key;
    if(m_detailTabs->currentIndex()==0)m_composer->editor()->setFocus(Qt::OtherFocusReason);
    m_clearRequest=m_client.requestClearContext(host,name,run,conversation,dismiss);
    renderDetails();
}

bool SessionsWindow::canStartStopped(const Entry &entry) const
{
    const auto &s = entry.session;
    return entry.online && (s.state == "paused" || s.state == "stopped") && s.tracked
        && QStringList{"codex", "claude", "kimi"}.contains(s.cmd) && !isTerminating(entry)
        && !m_pending && !m_settingsRequest && m_renameKey.isEmpty() && m_startSend.key.isEmpty()
        && !m_compacts.contains(entry.key) && !m_composer->isSending(entry.key);
}

void SessionsWindow::startFresh()
{
    const auto *entry = selected();
    if (!entry || !canStartStopped(*entry) || findChild<QMessageBox *>("startFreshConfirm")) return;
    const Entry original = *entry;
    QMessageBox dialog(QMessageBox::Warning, tr("Start fresh?"),
        tr("Start %1 on %2 with a new conversation?\n\nThe agent starts in the same folder with the same account and model, without the current conversation context. Earlier messages stay readable in Activity and in the agent’s native history, but Resume will no longer continue them.")
            .arg(original.session.name, original.machine), QMessageBox::NoButton, this);
    dialog.setObjectName("startFreshConfirm"); dialog.setTextFormat(Qt::PlainText);
    auto *confirm = dialog.addButton(tr("Start fresh"), QMessageBox::DestructiveRole);
    auto *cancel = dialog.addButton(QMessageBox::Cancel); dialog.setDefaultButton(cancel); dialog.setEscapeButton(cancel);
    dialog.exec();
    if (dialog.clickedButton() != confirm || !LocalSessionHost::confirmLaunch(this, m_fleet.local(), original.host)) return;
    entry = selected();
    if (!entry || entry->key != original.key || entry->session.runId != original.session.runId
        || entry->session.conversationId != original.session.conversationId || !canStartStopped(*entry)) {
        showNotice(tr("The session changed. Open its actions again."), true); return;
    }
    m_pending = true; m_restoreKey.clear();
    m_client.startFreshSession(entry->host, entry->session.name, entry->session.runId);
    showNotice(tr("Starting %1 with a new conversation…").arg(entry->session.name)); renderDetails();
}

void SessionsWindow::startToSend(const QString &key, const QString &text, const QList<MessageAttachment> &attachments)
{
    const auto *entry = selected();
    if (!entry || entry->key != key || !canStartStopped(*entry) || findChild<QMessageBox *>("startToSendConfirm")) return;
    const Entry original = *entry;
    const QString context = m_contextUsage->text();
    QMessageBox dialog(QMessageBox::Question, tr("Start the session to send?"),
        tr("%1 is stopped. How should it start before your message is sent?").arg(original.session.name), QMessageBox::NoButton, this);
    dialog.setObjectName("startToSendConfirm"); dialog.setTextFormat(Qt::PlainText);
    dialog.setInformativeText(tr("Resume continues the saved conversation, and the message is sent with its full context%1.\n\n"
        "Start fresh begins a new conversation with the same folder, account and model. Earlier messages stay readable in Activity.")
        .arg(context.isEmpty() ? QString() : tr(" (%1)").arg(context)));
    dialog.setInformativeText(dialog.informativeText()+tr("\n\nResume, compact and send summarizes the saved history first. Compaction uses tokens and may omit details."));
    auto *resume = dialog.addButton(tr("Resume and send"), QMessageBox::AcceptRole);
    resume->setObjectName("resumeAndSend"); resume->setEnabled(original.session.resumable);
    auto *compact = dialog.addButton(tr("Resume, compact and send"), QMessageBox::ActionRole);
    compact->setObjectName("resumeCompactAndSend"); compact->setEnabled(original.session.resumable);
    compact->setToolTip(tr("Resume the saved conversation, run native /compact when ready, then send this exact message after successful completion."));
    auto *fresh = dialog.addButton(tr("Start fresh and send"), QMessageBox::ActionRole); fresh->setObjectName("startFreshAndSend");
    auto *cancel = dialog.addButton(QMessageBox::Cancel); dialog.setDefaultButton(cancel); dialog.setEscapeButton(cancel);
    dialog.exec();
    const bool startFresh = dialog.clickedButton() == fresh;
    const bool compactFirst = dialog.clickedButton() == compact;
    if (!startFresh && !compactFirst && dialog.clickedButton() != resume) return;
    if (!LocalSessionHost::confirmLaunch(this, m_fleet.local(), original.host)) return;
    entry = selected();
    if (!entry || entry->key != key || entry->session.runId != original.session.runId
        || entry->session.conversationId != original.session.conversationId || !canStartStopped(*entry)) {
        showNotice(tr("The session changed. Your draft was kept without sending."), true); return;
    }
    if (!m_composer->draftMatches(key, text, attachments)) { showNotice(tr("Draft changed. Send it when you are ready.")); return; }
    m_startSend = {key, entry->host, entry->session.name, entry->session.runId, entry->session.conversationId,
        text, attachments, startFresh, compactFirst, QDateTime::currentMSecsSinceEpoch()};
    m_compactTimer.start();
    const Entry target = *entry;
    const auto serial = ++m_waitingPreparationSerial; m_waitingPreparations.insert(key, serial);
    beginSendEvidence(key, m_details);
    m_composer->prepareWaitingToSendAsync(key, compactFirst
        ? tr("Waiting for the session to start, then compact. This exact message will be sent automatically after compaction. Cancel send to edit.")
        : tr("Waiting for the session to start. This exact message will be sent automatically when ready. Cancel send to edit."),
        [this, key, target, startFresh, serial](bool okay) {
            const auto evidence = key == m_selectedKey ? sendEvidence(m_details) : m_pendingSendEvidence.value(key);
            finishSendEvidence(key);
            if (m_waitingPreparations.value(key) != serial || m_startSend.key != key) return;
            m_waitingPreparations.remove(key);
            const auto current = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &value) { return value.key == key; });
            if (!okay || current == m_entries.cend() || current->host != target.host || current->session.name != target.session.name
                || current->session.created != target.session.created || current->session.runId != target.session.runId
                || current->session.conversationId != target.session.conversationId || !current->online || isTerminating(*current)
                || (!evidence.isEmpty() && (evidence.value("run_id").toString() != target.session.runId
                    || evidence.value("conversation_id").toString() != target.session.conversationId))
                || current->session.state == "running" || m_pending || !m_renameKey.isEmpty()) {
                cancelStartContinuation(tr("Starting was cancelled. Your draft was kept without sending.")); return;
            }
            m_pending = true; m_restoreKey.clear();
            if (startFresh) m_client.startFreshSession(target.host, target.session.name, target.session.runId);
            else m_client.resumeSession(target.host, target.session.name);
            renderDetails();
        });
    renderDetails();
}

void SessionsWindow::cancelStartContinuation(const QString &reason)
{
    if (m_startSend.key.isEmpty()) return;
    const auto key = m_startSend.key; m_startSend = {}; m_waitingPreparations.remove(key);
    if (m_compacts.isEmpty()) m_compactTimer.stop();
    m_composer->deliveryFinished(key, false, reason);
}

// Send the waiting draft once the new run accepts input. A resume must keep
// its conversation; a fresh start must never reopen the replaced one.
void SessionsWindow::continueAfterStart(const QString &host, const QString &name, const QJsonObject &data)
{
    if (m_waitingPreparations.contains(m_startSend.key)) return;
    if (m_startSend.key.isEmpty() || m_startSend.host != host || m_startSend.name != name) return;
    if (!m_composer->draftMatches(m_startSend.key, m_startSend.text, m_startSend.attachments)) {
        cancelStartContinuation(tr("Draft changed. Send it when you are ready.")); return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_startSend.started > 180000) {
        cancelStartContinuation(tr("The agent did not become ready. Check Terminal; your draft was kept without sending.")); return;
    }
    const auto run = data.value("run_id").toString(), conversation = data.value("conversation_id").toString();
    if (m_pending || run.isEmpty() || run == m_startSend.run) return;
    const bool sameConversation = conversation == m_startSend.conversation;
    if (m_startSend.fresh ? sameConversation : !conversation.isEmpty() && !sameConversation) {
        cancelStartContinuation(tr("The agent opened an unexpected conversation. Your draft was kept without sending.")); return;
    }
    if (!data.value("tracked").toBool() || data.value("runtime_state")!="live" || data.value("process_state")!="running"
        || !data.value("error").toString().isEmpty() || data.value("phase")!="idle" || data.value("activity")!="idle"
        || m_terminating.contains(m_startSend.key)
        || (!data.value("expected_id").toString().isEmpty() && !data.value("resume_message_can_send").toBool())
        || (conversation.isEmpty() && !data.value("first_message_can_send").toBool())) return;
    const auto pending = m_startSend;
    if (pending.compact && (conversation.isEmpty() || !data.value("compact_context_supported").toBool())) return;
    m_startSend = {};
    if (pending.compact) compactToSend(pending.key,pending.host,pending.name,run,conversation,pending.text,pending.attachments);
    else deliverMessage(pending.key,pending.host,pending.name,run,conversation,pending.text,pending.attachments,{},{},true,data);
    if (m_compacts.isEmpty()) m_compactTimer.stop();
}

void SessionsWindow::sendMessage(const QString &key, const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId, const QString &compactionId, bool contextChosen)
{
    const auto *entry = selected();
    if (!entry || key != m_selectedKey || m_composer->isSending(key)) return;
    if (retryId.isEmpty() && compactionId.isEmpty() && !contextChosen && canStartStopped(*entry)) { startToSend(key, text, attachments); return; }
    const auto reason = messageBlockReason(*entry);
    if (!reason.isEmpty()) { m_composer->deliveryFinished(key, false, reason); return; }
    const QString runId = m_details.value("run_id").toString();
    const QString conversationId = m_details.value("conversation_id").toString();
    bool fullContextConsent = contextChosen || !compactionId.isEmpty();
    // Resume and send already chose the full saved context.
    if(compactionId.isEmpty() && !contextChosen && CacheStatus::expired(m_details)) {
        const auto host=entry->host,name=entry->session.name;
        QMessageBox dialog(QMessageBox::Warning,tr("Continue with a cold cache?"),CacheStatus::warning(m_details)+"\n\n"+
            tr("The next message may process the entire conversation again, increasing token usage, cost or subscription usage. Send with the full history, compact it into a summary before sending, or clear it and keep your draft. Compaction also uses tokens and may omit details."),QMessageBox::NoButton,this);
        dialog.setObjectName("coldCacheConfirm");auto *proceed=dialog.addButton(tr("Send with full context"),QMessageBox::AcceptRole);
        auto *compact=dialog.addButton(tr("Compact and continue"),QMessageBox::ActionRole);
        compact->setEnabled(m_details.value("compact_context_supported").toBool() && !m_compacts.contains(key));
        compact->setToolTip(tr("Run native /compact, then send this draft only after successful completion."));
        auto *clear=dialog.addButton(tr("Clear context"),QMessageBox::ActionRole);
        clear->setEnabled(m_details.value("clear_context_supported").toBool() && !m_clearRequest);
        // Name what the host reports blocking each unavailable action, such as
        // a subagent or tool that is still working.
        const auto compactReason=contextActionReason(m_details,"compact"),clearReason=contextActionReason(m_details,"clear");
        explainContextAction(compact,compactReason);explainContextAction(clear,clearReason);
        QStringList unavailable;
        if(!compactReason.isEmpty() && compactReason==clearReason)
            unavailable<<tr("Compact and continue and Clear context are unavailable. %1").arg(contextReasonSentence(compactReason));
        else {
            if(!compactReason.isEmpty())unavailable<<tr("Compact and continue is unavailable. %1").arg(contextReasonSentence(compactReason));
            if(!clearReason.isEmpty())unavailable<<tr("Clear context is unavailable. %1").arg(contextReasonSentence(clearReason));
        }
        // Reported names stay literal: the format also applies to this line.
        if(!unavailable.isEmpty()){dialog.setInformativeText(unavailable.join('\n'));dialog.setTextFormat(Qt::PlainText);}
        auto *cancel=dialog.addButton(QMessageBox::Cancel);dialog.setDefaultButton(cancel);dialog.setEscapeButton(cancel);dialog.exec();
        if(dialog.clickedButton()!=proceed && dialog.clickedButton()!=clear && dialog.clickedButton()!=compact)return;
        entry=selected();
        if(!entry||m_selectedKey!=key||entry->host!=host||entry->session.name!=name||m_details.value("run_id")!=runId||m_details.value("conversation_id")!=conversationId)return;
        if(dialog.clickedButton()==clear){clearContext();return;}
        if(dialog.clickedButton()==compact) {
            if (!m_composer->draftMatches(key, text, attachments)) { showNotice(tr("Draft changed. Send it when you are ready.")); return; }
            compactToSend(key,host,name,runId,conversationId,text,attachments,retryId);
            renderDetails(); return;
        }
        fullContextConsent = true;
        const auto currentProblem=messageBlockReason(*entry);if(!currentProblem.isEmpty()){showNotice(currentProblem,true);return;}
    }
    deliverMessage(key,entry->host,entry->session.name,runId,conversationId,text,attachments,retryId,compactionId,fullContextConsent);
}

void SessionsWindow::deliverMessage(const QString &key, const QString &host, const QString &name, const QString &runId, const QString &conversationId,
                                   const QString &text, const QList<MessageAttachment> &attachments, const QString &retryId, const QString &compactionId, bool fullContextConsent, const QJsonObject &inspectionEvidence)
{
    if (m_composer->isSending(key)) return;
    const auto original = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &entry) { return entry.key == key && entry.host == host && entry.session.name == name; });
    if (original == m_entries.cend()) return;
    const Entry target = *original;
    QJsonObject retryMessage;
    if (!retryId.isEmpty()) {
        for (const auto &value : m_localMessages.value(key)) {
            const auto message = value.toObject();
            if (message.value("id").toString() == retryId && message.value("status") == "error" && !message.value("uncertain").toBool()) {
                retryMessage = message; break;
            }
        }
        if (retryMessage.isEmpty()) return;
    }
    const QJsonObject cacheEvidence = !inspectionEvidence.isEmpty() ? sendEvidence(inspectionEvidence)
        : key == m_selectedKey ? sendEvidence(m_details) : QJsonObject();
    beginSendEvidence(key, cacheEvidence);
    const bool preserve = !retryId.isEmpty() && !m_composer->draftMatches(key, text, attachments);
    m_composer->prepareSendingAsync(key, preserve, [this, target, key, host, name, runId, conversationId, text, attachments, retryId, retryMessage, compactionId, fullContextConsent, cacheEvidence](bool okay) {
        if (!okay) { finishSendEvidence(key); return; }
        const auto stillValid = [this, target, key, host, name, runId, conversationId, retryId, retryMessage, fullContextConsent, cacheEvidence] {
            const auto current = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &entry) { return entry.key == key; });
            const auto evidence = m_pendingSendEvidence.value(key, cacheEvidence);
            if (!fullContextConsent && (CacheStatus::expired(cacheEvidence) || CacheStatus::expired(evidence))) return false;
            if (current == m_entries.cend() || current->host != host || current->session.name != name
                || current->session.created != target.session.created
                || !observedIdentityMatches(current->session, target.session, runId, conversationId)
                || !current->online || isTerminating(*current)) return false;
            if (!evidence.isEmpty() && (evidence.value("run_id").toString() != runId
                || evidence.value("conversation_id").toString() != conversationId
                || evidence.value("runtime_state") != "live" || evidence.value("process_state") != "running")) return false;
            if (m_pending || !m_renameKey.isEmpty() || (key == m_clearKey && m_clearRequest)
                || (key == m_settingsKey && m_settingsRequest)) return false;
            if (!retryId.isEmpty()) {
                bool retained = false;
                for (const auto &value : m_localMessages.value(key)) if (value.toObject() == retryMessage) { retained = true; break; }
                if (!retained) return false;
            }
            if (key == m_selectedKey && (m_details.value("run_id").toString() != runId
                || m_details.value("conversation_id").toString() != conversationId || !messageBlockReason(*current).isEmpty()
                || (CacheStatus::expired(m_details) && !fullContextConsent))) return false;
            return true;
        };
        if (!stillValid()) { finishSendEvidence(key); m_composer->deliveryFinished(key, false, tr("The session changed. Your draft was kept without sending.")); return; }
        AttachmentFiles::prepareAsync(attachments, this,
            [this, stillValid, key, host, name, runId, conversationId, text, attachments, retryId, compactionId](const QJsonArray &files, const QString &error) {
                const bool valid = stillValid(); finishSendEvidence(key);
                if (!error.isEmpty() || !valid || !m_composer->isSending(key)) {
                    m_composer->deliveryFinished(key, false, error.isEmpty() ? tr("The session changed. Your draft was kept without sending.") : error); return;
                }
                const quint64 request = m_client.requestSendMessage(host, name, text, attachments, runId, conversationId, {}, compactionId);
                m_pendingMessages.insert(request, {key, runId, conversationId});
                if (m_push && m_push->key == key && !m_push->request && m_push->text == text) {
                    m_push->request = request; m_push->run = runId; m_push->conversation = conversationId;
                }
                auto &messages = m_localMessages[key];
                for(int i=messages.size();i-- >0;) {
                    const auto previous=messages[i].toObject();
                    if(previous.value("status")=="error" && (previous.value("id").toString()==retryId
                        || (previous.value("text").toString()==text && previous.value("attachments").toArray()==files)))messages.removeAt(i);
                }
                messages.append(QJsonObject{{"id", QString::number(request)}, {"text", text}, {"status", "sending"},
                    {"attachments", files}, {"run_id", runId}, {"conversation_id", conversationId},
                    {"submitted_at", QDateTime::currentMSecsSinceEpoch() / 1000.0}});
                while (messages.size() > 50) messages.removeFirst();
                if (key == m_selectedKey) { renderDetails(); m_activityView->jumpToLatest(); }
            });
    });
}

void SessionsWindow::finishMessage(quint64 request, bool ok, const QJsonObject &receipt, const QString &error, bool uncertain)
{
    if (m_childMessages.contains(request)) {
        const auto key = m_childMessages.take(request);
        m_subagentComposer->deliveryFinished(key, ok, error, uncertain);
        inspect(); emit refreshRequested(); return;
    }
    if (!m_pendingMessages.contains(request)) return;
    const auto pending = m_pendingMessages.take(request);
    const QString detail = uncertain ? tr("Delivery is not confirmed. Check Terminal before retrying. %1").arg(error) : error;
    m_composer->deliveryFinished(pending.key, ok, detail, uncertain);
    pushSendFinished(request, ok, uncertain);
    auto &messages = m_localMessages[pending.key];
    for (qsizetype i = 0; i < messages.size(); ++i) {
        auto message = messages.at(i).toObject();
        if (message.value("id").toString() != QString::number(request)) continue;
        message["status"] = ok ? "sent" : "error"; message["error"] = detail;
        message["uncertain"] = !ok && uncertain;
        if (ok) {
            message["message_id"]=receipt.value("request_id");
            auto files=message.value("attachments").toArray();
            for(int j=0;j<files.size();++j){auto file=files[j].toObject();file["request_id"]=receipt.value("request_id");file["index"]=j;files[j]=file;}
            message["attachments"]=files;
            message["submitted_text"] = receipt.value("submitted_text");
            if (receipt.contains("submitted_at")) message["submitted_at"] = receipt.value("submitted_at");
        }
        messages[i] = message; break;
    }
    if (pending.key == m_selectedKey) { reconcileMessages(); renderDetails(); inspect(); }
    emit refreshRequested();
}

void SessionsWindow::reconcileMessages()
{
    auto &messages = m_localMessages[m_selectedKey];
    const QString runId = m_details.value("run_id").toString();
    const QString conversationId = m_details.value("conversation_id").toString();
    for (qsizetype i = messages.size(); i-- > 0;) {
        const auto message = messages.at(i).toObject();
        if (message.value("status").toString() == "sending") continue;
        if ((!runId.isEmpty() && message.value("run_id").toString() != runId)
            || (!conversationId.isEmpty() && message.value("conversation_id").toString() != conversationId)) {
            messages.removeAt(i); continue;
        }
        if(message.value("source")=="hgs_voice") {
            for(const auto &value:m_details.value("voice_messages").toArray())
                if(value.toObject().value("message_id")==message.value("message_id")){messages.removeAt(i);break;}
            continue;
        }
        const bool failed=message.value("status")=="error";
        if (!failed && message.value("status") != "sent") continue;
        bool saved=false;
        for(const auto &value:m_details.value("attachment_messages").toArray()) {
            if(!message.value("message_id").toString().isEmpty() && value.toObject().value("message_id")==message.value("message_id")){saved=true;break;}
        }
        if(saved){messages.removeAt(i);continue;}
        // Keep the local file card until the durable attachment metadata arrives.
        if(!message.value("attachments").toArray().isEmpty())continue;
        const QString submitted = message.value("submitted_text").toString(message.value("text").toString()).trimmed();
        if (submitted.isEmpty()) continue;
        for (const auto &value : m_events) {
            const auto event = value.toObject();
            if (!QStringList{"UserPromptSubmit", "UserPromptQueued", "TurnStarted"}.contains(event.value("type").toString())
                || !event.value("agent_id").toString().isEmpty()
                || event.value("at").toDouble() < message.value("submitted_at").toDouble() - 2) continue;
            const QString detail = event.value("detail").toString().trimmed();
            if (!detail.isEmpty() && (failed ? detail==submitted : detail.startsWith(submitted.left(1000)))) {
                if(failed && !m_composer->isSending(m_selectedKey) && m_composer->draftMatches(m_selectedKey,message.value("text").toString(),{}))
                    m_composer->deliveryFinished(m_selectedKey,true);
                messages.removeAt(i); break;
            }
        }
    }
}

void SessionsWindow::renderQuestion(const Entry &entry, int navigation)
{
    m_question->setNativeUi(entry.session.cmd=="dsh");
    m_question->setTerminalAvailable(entry.session.cmd=="dsh"||NativeSessionCapabilities::terminal(entry.session,m_details),NativeSessionCapabilities::terminalReason(entry.session,m_details));
    QJsonObject question;
    QList<QJsonObject> optional;
    int pendingQuestions=0;QSet<QString> seen;
    for (const auto &value : m_details.value("pending_questions").toArray()) {
        const auto candidate = value.toObject();
        const auto id=candidate.value("question_id").toString();
        if(id.isEmpty() || seen.contains(id) || candidate.value("questions").toArray().isEmpty())continue;
        seen.insert(id);
        if(m_question->hasSubmittedAnswer(m_selectedKey,candidate))continue;
        pendingQuestions+=candidate.value("questions").toArray().size();
        if(candidate.value("optional").toBool())optional.append(candidate);
        else if(question.isEmpty() || (!question.value("can_answer").toBool() && candidate.value("can_answer").toBool()))question=candidate;
    }
    std::stable_sort(optional.begin(),optional.end(),[](const QJsonObject &a,const QJsonObject &b) {
        return a.value("created_at").toDouble()>b.value("created_at").toDouble();
    });
    const QString scope=QString::fromUtf8(QJsonDocument(QJsonArray{m_selectedKey,m_details.value("run_id"),m_details.value("conversation_id")}).toJson(QJsonDocument::Compact));
    const QString chosen=m_questionSelections.value(scope);
    int index=0;
    if(!chosen.isEmpty()) {
        const auto found=std::find_if(optional.cbegin(),optional.cend(),[&](const QJsonObject &item){return item.value("question_id").toString()==chosen;});
        if(found==optional.cend())m_questionSelections.remove(scope);
        else index=int(found-optional.cbegin());
    }
    // Required confirmations always take priority. Optional requests can be
    // answered in any order; polling must preserve explicit queue navigation.
    if(question.isEmpty() && !optional.isEmpty()) {
        if(navigation) {
            index=qBound(0,index+navigation,int(optional.size())-1);
            m_questionSelections.insert(scope,optional[index].value("question_id").toString());
        }
        question=optional[index];
    }
    const bool available = !question.isEmpty();
    m_nativeUi->setResponsePending(entry.session.cmd == "dsh" && available);
    auto *focus = QApplication::focusWidget();
    const bool returnToComposer = !available && m_question->isVisible() && focus
        && (focus == m_question || m_question->isAncestorOf(focus));
    // Show and focus the replacement before clearing the focused question's
    // children. Background completion must not interrupt navigation elsewhere.
    m_composer->setInputVisible(!available || question.value("optional").toBool());
    if (returnToComposer) m_composer->editor()->setFocus(Qt::OtherFocusReason);
    m_question->setQuestion(m_selectedKey, question, pendingQuestions);
    m_question->setQueueNavigation(index,int(optional.size()));
    m_question->setVisible(available);
    QString reason;
    if (!entry.online) reason = tr("Machine unavailable. Your answers stay here.");
    else if (entry.session.state != "running") reason = tr("This session is not running.");
    else if (isTerminating(entry) || m_pending || !m_renameKey.isEmpty()) reason = tr("Wait for the session update to finish.");
    else if (!m_inspectError.isEmpty()) reason = tr("Refresh the session before answering.");
    else if (question.value("run_id") != m_details.value("run_id")
             || question.value("conversation_id") != m_details.value("conversation_id"))
        reason = tr("Refresh the pending question before answering.");
    else if (m_details.value("runtime_state").toString() != "live"
             || m_details.value("process_state").toString() != "running")
        reason = tr("The agent is no longer running.");
    m_question->setAvailability(reason.isEmpty(), reason);
    if (available && reason.isEmpty() && question.value("can_answer").toBool())
        m_hint->hide();
}

void SessionsWindow::answerQuestion(const QString &key, const QString &questionId, const QJsonArray &answers)
{
    // Signals can refer to QuestionCard members which a nested modal event loop
    // replaces. Consent and transport must use immutable copies instead.
    const QString targetKey = key, targetQuestionId = questionId;
    const QJsonArray requestedAnswers = answers;
    if (m_confirmingQuestionCache) return;
    const bool skipping = !requestedAnswers.isEmpty() && requestedAnswers.first().toObject().value("skip").toBool();
    const auto actionableQuestion = [&](bool reportChange = false) -> QJsonObject {
        const auto *entry = selected();
        if (!entry || targetKey != m_selectedKey || !entry->online || entry->session.state != "running"
            || isTerminating(*entry) || m_pending || !m_renameKey.isEmpty() || !m_inspectError.isEmpty()
            || m_details.value("runtime_state").toString() != "live"
            || m_details.value("process_state").toString() != "running") return {};
        for (const auto &pending : m_pendingAnswers) if (pending.key == targetKey) return {};
        QJsonObject question;
        for (const auto &value : m_details.value("pending_questions").toArray())
            if (value.toObject().value("question_id").toString() == targetQuestionId) { question = value.toObject(); break; }
        if (m_question->hasSubmittedAnswer(targetKey, question)) return {};
        if (question.isEmpty() || !(skipping ? question.value("can_skip").toBool() : question.value("can_answer").toBool())
            || question.value("run_id") != m_details.value("run_id")
            || question.value("conversation_id") != m_details.value("conversation_id")) {
            if (reportChange) m_question->setError(targetKey, targetQuestionId, tr("This question changed. Refresh before answering."));
            return {};
        }
        return question;
    };
    const auto consentIdentity = [](const QJsonObject &question) {
        QJsonObject identity;
        for (const auto *field : {"question_id", "question_hash", "run_id", "conversation_id", "questions",
                "source", "tool", "tool_name", "tool_call_id", "native_question_id", "agent_id",
                "answer_transport", "approval", "approval_choices", "trust_request", "optional"}) {
            const auto name = QString::fromLatin1(field);
            if (question.contains(name)) identity.insert(name, question.value(name));
        }
        return identity;
    };
    const auto question = actionableQuestion(true);
    if (question.isEmpty()) return;
    // This reviewed native panel already asks the user how to handle its cold
    // cache. Deliver the chosen row, without asking to resume with full context.
    const bool nativeCacheChoice = question.value("source") == "kimi_cache_hint"
        && question.value("answer_transport") == "kimi_tui";
    bool fullContextConsent = false;
    if (!nativeCacheChoice && CacheStatus::expired(m_details)) {
        const Entry original = *selected();
        const auto run = m_details.value("run_id"), conversation = m_details.value("conversation_id");
        const auto draft = m_question->submissionSnapshot(targetKey, targetQuestionId);
        if (draft.isEmpty() || draft.value("sending").toBool() || draft.value("submitted").toBool()
            || draft.value("answered").toBool() || draft.value("uncertain").toBool()
            || !draft.value("storage_error").toString().isEmpty()) return;
        QScopedValueRollback<bool> confirming(m_confirmingQuestionCache, true);
        QMessageBox box(QMessageBox::Warning, tr("Submit with a cold cache?"),
            CacheStatus::warning(m_details) + "\n\n" + tr("Submitting this answer may resume the agent with the entire conversation, increasing token usage, cost or subscription usage. Compaction or clearing can invalidate the pending question. Cancel keeps your answers."),
            QMessageBox::NoButton, this);
        box.setObjectName("questionColdCacheConfirm");
        auto *proceed = box.addButton(tr("Submit with full context"), QMessageBox::AcceptRole);
        auto *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel); box.setEscapeButton(cancel);
        box.exec();
        if (box.clickedButton() != proceed) return;
        const auto *current = selected();
        if (!current || current->key != original.key || current->host != original.host
            || current->session.name != original.session.name || current->session.created != original.session.created
            || current->session.runId != original.session.runId || current->session.conversationId != original.session.conversationId
            || m_details.value("run_id") != run || m_details.value("conversation_id") != conversation
            || consentIdentity(actionableQuestion()) != consentIdentity(question)
            || m_question->submissionSnapshot(targetKey, targetQuestionId) != draft) {
            showNotice(tr("The question or your answers changed. Your draft was kept; review it before submitting again."), true);
            return;
        }
        fullContextConsent = true;
    }
    const auto *entry = selected();
    if (!entry) return;
    const Entry target = *entry;
    const auto pinnedIdentity = consentIdentity(question);
    const QJsonObject cacheEvidence = sendEvidence(m_details);
    beginSendEvidence(targetKey, cacheEvidence);
    m_question->prepareSendingAsync(targetKey, targetQuestionId,
        [this, target, targetKey, targetQuestionId, question, requestedAnswers, pinnedIdentity, skipping, consentIdentity, nativeCacheChoice, fullContextConsent, cacheEvidence](bool okay) {
            if (!okay) { finishSendEvidence(targetKey); return; }
            const auto current = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &value) { return value.key == targetKey; });
            const auto evidence = targetKey == m_selectedKey ? sendEvidence(m_details) : m_pendingSendEvidence.value(targetKey, cacheEvidence);
            finishSendEvidence(targetKey);
            QJsonObject refreshed;
            for (const auto &value : evidence.value("pending_questions").toArray())
                if (value.toObject().value("question_id").toString() == targetQuestionId) { refreshed = value.toObject(); break; }
            if (current == m_entries.cend() || current->host != target.host || current->session.name != target.session.name
                || current->session.created != target.session.created
                || !observedIdentityMatches(current->session, target.session, question.value("run_id").toString(), question.value("conversation_id").toString())
                || !current->online || current->session.state != "running" || isTerminating(*current)
                || m_pending || !m_renameKey.isEmpty() || (targetKey == m_selectedKey && !m_inspectError.isEmpty())
                || evidence.value("runtime_state") != "live" || evidence.value("process_state") != "running"
                || evidence.value("run_id") != question.value("run_id") || evidence.value("conversation_id") != question.value("conversation_id")
                || consentIdentity(refreshed) != pinnedIdentity
                || (!nativeCacheChoice && !fullContextConsent && (CacheStatus::expired(cacheEvidence) || CacheStatus::expired(evidence)))
                || !(skipping ? refreshed.value("can_skip").toBool() : refreshed.value("can_answer").toBool())
                || m_question->hasSubmittedAnswer(targetKey, refreshed)) {
                m_question->setError(targetKey, targetQuestionId, tr("The question changed. Your answers were kept without submitting.")); return;
            }
            const auto request = m_client.requestAnswerQuestion(target.host, target.session.name, question, requestedAnswers);
            m_pendingAnswers.insert(request, {targetKey, targetQuestionId, question.value("question_hash").toString()});
            renderDetails();
        });
}

void SessionsWindow::finishQuestion(quint64 request, bool ok, const QString &error, bool uncertain, bool submitted)
{
    if (!m_pendingAnswers.contains(request)) return;
    const auto pending = m_pendingAnswers.take(request);
    if (ok && submitted) m_question->setSubmitted(pending.key, pending.questionId);
    else if (ok) m_question->setAnswered(pending.key, pending.questionId);
    else m_question->setError(pending.key, pending.questionId, error, uncertain);
    if (m_selectedKey == pending.key) { renderDetails(); inspect(); }
    emit refreshRequested();
}

void SessionsWindow::openExternalFile(const QString &path, bool otherMachine)
{
    switch (ExternalFile::open(path, otherMachine ? ExternalFile::Origin::OtherMachine : ExternalFile::Origin::ThisComputer)) {
    case ExternalFile::Outcome::Opened: return;
    case ExternalFile::Outcome::Revealed: showNotice(ExternalFile::revealedNotice(path)); return;
    case ExternalFile::Outcome::RevealFailed: showNotice(ExternalFile::revealFailedNotice(path), true); return;
    case ExternalFile::Outcome::OpenFailed:
        showNotice(tr("Could not open %1 with its default application. You can copy the path.").arg(path), true); return;
    }
}

void SessionsWindow::showNotice(const QString &message, bool error)
{
    m_notice->show();
    m_noticeDismiss->show();
    m_notice->setText(message.isEmpty() ? tr("Operation failed. Open the terminal for details.") : message);
    m_notice->setToolTip(Qt::convertFromPlainText(m_notice->text()));
    m_notice->setProperty("error", error);
    m_notice->style()->unpolish(m_notice); m_notice->style()->polish(m_notice); m_notice->update();
    const int revision=m_notice->property("noticeRevision").toInt()+1;
    m_notice->setProperty("noticeRevision",revision);
    updateSessionFeedback();
    if (!error) QTimer::singleShot(m_noticeMs, this, [this,revision] {
        if (m_notice->property("noticeRevision").toInt()==revision) clearNotice();
    });
}

void SessionsWindow::clearNotice()
{
    m_notice->setProperty("noticeRevision", m_notice->property("noticeRevision").toInt() + 1);
    m_notice->clear(); m_notice->setToolTip({}); m_notice->hide(); m_noticeDismiss->hide();
    updateSessionFeedback();
}

bool SessionsWindow::selectSessionEntry(const Entry &original)
{
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [&original](const Entry &entry) { return entry.key == original.key; });
    if (found == m_entries.cend() || (original.session.created > 0 && found->session.created > 0 && original.session.created != found->session.created)) {
        showNotice(tr("This session is no longer available. Refresh the list and open its actions again."), true); return false;
    }
    for (int row = 0; row < m_sessions->count(); ++row) if (m_sessions->item(row)->data(KeyRole).toString() == original.key) {
        m_sessions->setCurrentRow(row); renderDetails(); return true;
    }
    showNotice(tr("This session is no longer in the current view. Open its actions again."), true); return false;
}

QString SessionsWindow::selectedDirectory() const
{
    const auto *entry = selected(); if (!entry) return {};
    return m_details.value("cwd").toString(entry->session.cwd);
}

void SessionsWindow::openFolderShell()
{
    const auto *entry = selected();
    if (!entry || !entry->online) return;
    const auto directory = m_subagentId.isEmpty() ? selectedDirectory() : m_subagentCwd;
    if (MachinePlatform::absolute(directory, MachinePlatform::of(m_fleet.local().metrics, entry->host.isEmpty())))
        emit folderShellRequested(entry->host, directory);
}

void SessionsWindow::openSessionFolder()
{
    const auto *entry = selected();
    if (!entry || !entry->host.isEmpty()) return;
    openLocalFolder(m_subagentId.isEmpty() ? selectedDirectory() : m_subagentCwd);
}

void SessionsWindow::openLocalFolder(const QString &directory)
{
    const QFileInfo folder(directory);
    if (!folder.isAbsolute() || !folder.isDir()) {
        showNotice(tr("Session folder is unavailable."), true); return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory)))
        showNotice(tr("Could not open the session folder in the file manager."), true);
}

void SessionsWindow::showSessionMenu(const QString &key, const QPoint &position)
{
    if (selectionEntries().size() > 1) { showSelectionMenu(position); return; }
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [&key](const Entry &entry) { return entry.key == key; });
    if (found == m_entries.cend()) return;
    const Entry original = *found;
    const bool archived = original.session.state == "archived";
    const auto &target = original.session;
    const bool selectedTarget = original.key == m_selectedKey && m_subagentId.isEmpty();
    const bool idle = !isTerminating(original) && !m_pending && !m_settingsRequest && m_renameKey.isEmpty()
        && !m_composer->isSending(original.key) && std::none_of(m_pendingAnswers.cbegin(), m_pendingAnswers.cend(), [&original](const PendingAnswer &p) { return p.key == original.key; });
    const bool available = original.online && idle;
    const QString directory = selectedTarget ? selectedDirectory() : target.cwd;
    const bool canPause = target.state == "running" && target.resumable && target.activity == "idle" && target.processState != "exited";
    const bool canChange = available && (archived ? target.resumable && !target.archiveId.isEmpty() && !archiveNameOccupied(original)
        : canPause || (target.state != "running" && target.resumable));
    const auto add = [this, original](const QString &text, const char *name, SessionMenuAction kind, bool enabled, const QString &hint = QString()) {
        auto *action = m_sessionMenu->addAction(text); action->setObjectName(QLatin1String(name)); action->setEnabled(enabled);
        if (!hint.isEmpty()) action->setToolTip(hint);
        connect(action, &QAction::triggered, this, [this, original, kind]() { runSessionMenuAction(original, kind); });
        return action;
    };
    qDeleteAll(m_sessionMenu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    m_sessionMenu->clear();
    if (!archived) add(original.session.cmd == "dsh" ? tr("Open native DeepSeek UI") : tr("Open terminal"), "contextOpenTerminal", SessionMenuAction::OpenTerminal, available && (target.cmd=="dsh"||NativeSessionCapabilities::terminal(target,selectedTarget?m_details:QJsonObject())),
        NativeSessionCapabilities::terminal(target,selectedTarget?m_details:QJsonObject())||target.cmd=="dsh"?QString():NativeSessionCapabilities::terminalReason(target,selectedTarget?m_details:QJsonObject()));
    if (!archived && SessionPresentation::backgroundIdle(original.session))
        add(tr("Show running commands"), "contextOpenProcesses", SessionMenuAction::OpenProcesses, original.online && ProcessSettings::enabled(),
            ProcessSettings::enabled() ? tr("Follow this session's background work in Processes") : tr("Enable Settings > Processes to inspect running commands"));
    const bool localFolder = original.host.isEmpty() && QFileInfo(directory).isAbsolute() && QFileInfo(directory).isDir();
    add(tr("Open folder"), "contextOpenFolder", SessionMenuAction::OpenFolder, localFolder,
        !original.host.isEmpty() ? tr("Available only for sessions on this computer")
        : localFolder ? tr("Open session folder in file manager\n%1").arg(directory) : tr("Session folder is unavailable"));
    add(tr("Open shell in session folder"), "contextFolderShell", SessionMenuAction::FolderShell,
        original.online && MachinePlatform::absolute(directory, MachinePlatform::of(m_fleet.local().metrics, original.host.isEmpty())));
    add(archived ? tr("Copy restore command") : tr("Copy connect command"), "contextCopyCommand", SessionMenuAction::CopyCommand,
        (archived || NativeSessionCapabilities::terminal(target)) && (!archived || !original.session.archiveId.isEmpty()));
    if (!archived) {
        m_sessionMenu->addSeparator();
        if (entryNeedsAttention(original))
            add(tr("Mark as read"), "contextMarkRead", SessionMenuAction::MarkRead, true);
        else
            add(tr("Mark needs attention"), "contextReviewLater", SessionMenuAction::ReviewLater, true,
                tr("Keep a reminder to review this session later"));
    }
    m_sessionMenu->addSeparator();
    add(archived ? tr("Restore session") : original.session.state == "running" ? tr("Pause session") : tr("Resume session"),
        "contextChangeSession", SessionMenuAction::ChangeState, selectedTarget ? m_pause->isEnabled() : canChange && NativeSessionCapabilities::action(target,archived?"restore":target.state=="running"?"pause":"resume"));
    if (target.state == "paused" || target.state == "stopped")
        add(tr("Start fresh…"), "contextStartFresh", SessionMenuAction::StartFresh, canStartStopped(original),
            tr("Start with a new conversation and the same folder, account and model"));
    add(tr("Rename…"), "contextRenameSession", SessionMenuAction::Rename, available && NativeSessionCapabilities::action(target,"rename") && !target.name.section('/', 1, 1).isEmpty() && (!archived || !target.archiveId.isEmpty()));
    auto *fork = add(tr("Fork session…"), "contextForkSession", SessionMenuAction::Fork, selectedTarget && m_forkAction->isEnabled(), m_forkAction->toolTip());
    if (!selectedTarget && available) {
        fork->setToolTip(tr("Checking whether this session can be forked…"));
        connect(&m_client, &HgsClient::inspectionReady, fork, [fork, original](const QString &host, const QString &name, const QJsonObject &data, const QString &archive) {
            if (host != original.host || name != original.session.name || archive != original.session.archiveId) return;
            if ((!original.session.runId.isEmpty() && data["run_id"] != original.session.runId)
                || (!original.session.conversationId.isEmpty() && data["conversation_id"] != original.session.conversationId)) return;
            fork->setProperty("details", data); fork->setEnabled(data["fork_supported"].toBool());
            fork->setToolTip(data["fork_reason"].toString());
        });
        m_client.requestInspection(original.host, target.name, 0, target.archiveId,false);
    }
    if (original.session.state == "paused" || original.session.state == "stopped")
        add(tr("Move to archive"), "contextArchiveSession", SessionMenuAction::Archive, available && NativeSessionCapabilities::action(target,"archive"),NativeSessionCapabilities::actionReason(target,"archive"));
    auto *groups = m_sessionMenu->addMenu(tr("Move to project")); groups->setObjectName("moveSessionToGroup");
    for (const auto &group : m_organization.groups()) {
        auto *action = groups->addAction(QString(group.name).replace('&', "&&")); action->setCheckable(true);
        action->setChecked(m_organization.groupFor(original.identity) == group.id); action->setData(group.id);
        connect(action, &QAction::triggered, this, [this, identity = original.identity, id = group.id]() {
            m_organization.moveSession(identity, id); saveOrganization(); rebuild();
        });
    }
    groups->addSeparator();
    connect(groups->addAction(tr("New project…")), &QAction::triggered, this, [this, identity = original.identity]() { createGroup(identity); });
    m_sessionMenu->addSeparator();
    add(tr("Copy session name"), "contextCopyName", SessionMenuAction::CopyName, true);
    add(tr("Copy session reference"), "contextCopyReference", SessionMenuAction::CopyReference, original.online && !target.runId.isEmpty());
    add(tr("Copy folder path"), "contextCopyFolder", SessionMenuAction::CopyFolder, !directory.isEmpty());
    m_sessionMenu->addSeparator();
    add(archived ? tr("Forget archived session…") : original.session.state == "running" ? tr("Terminate session…") : tr("Forget saved session…"),
        "contextTerminateSession", SessionMenuAction::Terminate, available && NativeSessionCapabilities::action(target,target.state=="running"?"terminate":"forget") && (!archived || !target.archiveId.isEmpty()),NativeSessionCapabilities::actionReason(target,target.state=="running"?"terminate":"forget"));
    m_sessionMenu->popup(position);
}

void SessionsWindow::copySessionReference(const Entry &entry, const QString &agent)
{
    const bool selectedTarget=entry.key==m_selectedKey;
    const auto run=selectedTarget?m_details.value("run_id").toString(entry.session.runId):entry.session.runId;
    const auto conversation=selectedTarget?m_details.value("conversation_id").toString(entry.session.conversationId):entry.session.conversationId;
    m_client.requestSessionReference(entry.host,entry.session.name,run,conversation,entry.session.archiveId,agent);
}

void SessionsWindow::runSessionMenuAction(const Entry &original, SessionMenuAction action)
{
    const auto found = std::find_if(m_entries.cbegin(), m_entries.cend(), [&original](const Entry &e) { return e.key == original.key; });
    if (found == m_entries.cend() || (original.session.created > 0 && found->session.created != original.session.created)
        || found->session.conversationId != original.session.conversationId) {
        showNotice(tr("This session is no longer available. Open its actions again."), true); return;
    }
    const Entry current = *found;
    const auto directory = current.key == m_selectedKey && m_subagentId.isEmpty() ? selectedDirectory() : current.session.cwd;
    switch (action) {
    case SessionMenuAction::MarkRead:
    case SessionMenuAction::ReviewLater: {
        const bool changed = action == SessionMenuAction::MarkRead ? m_fleet.markSessionRead(original.host, original.session)
            : m_fleet.setReviewLater(original.host, original.session, !original.session.reviewLater);
        if (!changed) {
            showNotice(action == SessionMenuAction::MarkRead
                ? tr("A new reply or question arrived. Open the menu again to mark it as read.")
                : tr("This session is no longer available. Open its actions again."), true);
            return;
        }
        ReadMarkStore::add(m_fleet.takeNewReadMarks());
        const auto marks = m_fleet.attentionMarks(), read = m_fleet.readReplies();
        QSettings().setValue("attention/sessionMarks", QJsonDocument(marks).toJson(QJsonDocument::Compact));
        QSettings().setValue("attention/readReplies", QJsonDocument(read).toJson(QJsonDocument::Compact));
        if (current.key == m_selectedKey) m_readSelectionKey = current.key;
        rebuild(); updateDashboard(); emit attentionMarksChanged(marks, read); return;
    }
    case SessionMenuAction::OpenFolder:
        if (current.host.isEmpty()) openLocalFolder(directory); return;
    case SessionMenuAction::FolderShell:
        if (current.online && MachinePlatform::absolute(directory, MachinePlatform::of(m_fleet.local().metrics, current.host.isEmpty())))
            emit folderShellRequested(current.host, directory);
        return;
    case SessionMenuAction::CopyName: emit copyTextRequested(current.session.name); return;
    case SessionMenuAction::CopyReference: copySessionReference(current); return;
    case SessionMenuAction::CopyFolder: if (!directory.isEmpty()) emit copyTextRequested(directory); return;
    case SessionMenuAction::CopyCommand:
        if (current.session.state != "archived" || !current.session.archiveId.isEmpty())
            emit copySessionCommandRequested(current.host, current.session.name, current.session.archiveId);
        return;
    default: break;
    }
    if (!selectSessionEntry(original)) return;
    const auto *entry = selected(); if (!entry) return;
    if (action == SessionMenuAction::Fork) {
        const auto *fork = m_sessionMenu->findChild<QAction *>("contextForkSession");
        const auto data = fork ? fork->property("details").toJsonObject() : QJsonObject();
        if (!data.isEmpty() && data["run_id"].toString() == entry->session.runId && data["conversation_id"].toString() == entry->session.conversationId) {
            m_details = data; renderDetails();
        }
    }
    switch (action) {
    case SessionMenuAction::OpenProcesses:
        if (entry->online && ProcessSettings::enabled()) m_detailTabs->setCurrentWidget(m_processes);
        return;
    case SessionMenuAction::OpenTerminal:
        if (entry->session.cmd == "dsh") { showNativeUi(*entry); return; }
        if (!entry->online || !m_open->isEnabled() || entry->session.state == "archived" || !NativeSessionCapabilities::terminal(entry->session,m_details)) return;
        // This terminal opens also in clipboard mode, and `hgs a` resumes a saved session.
        if (entry->session.state != "running") {
            if (!confirmLocalStart(*entry)) return;
            entry = selected();
            if (!entry->online || !m_open->isEnabled() || !NativeSessionCapabilities::terminal(entry->session,m_details)) return;
        }
        emit terminalOpenRequested(entry->host, entry->session.name,
            entry->host.isEmpty() && entry->session.attached && !entry->session.clients.isEmpty() ? entry->session.clients.first() : QString());
        return;
    case SessionMenuAction::ChangeState:
    case SessionMenuAction::Terminate:
        if ((original.session.state == "running") != (entry->session.state == "running")) {
            showNotice(tr("The session state changed. Open its actions again."), true); return;
        }
        if (action == SessionMenuAction::ChangeState) changeSession(); else terminateSession();
        return;
    case SessionMenuAction::Rename: renameSession(); return;
    case SessionMenuAction::StartFresh: startFresh(); return;
    case SessionMenuAction::Fork: forkSession(); return;
    case SessionMenuAction::Archive: archiveSession(); return;
    default: return;
    }
}

void SessionsWindow::jumpToLatestActivity(QListWidgetItem *item)
{
    if (!item || item->data(SessionRoles::Header).toBool() || !item->data(SessionRoles::LaunchId).toString().isEmpty()) return;
    m_detailTabs->setCurrentIndex(0);
    (m_subagentId.isEmpty() ? m_activityView : m_subagentView)->jumpToLatest();
}

void SessionsWindow::showNativeUi(const Entry &entry)
{
    if (NativeBrowserPlugin::factory()) { m_detailTabs->setCurrentWidget(m_nativeUi); return; }
    if (entry.online) m_client.openNativeUi(entry.host, entry.session.name);
}

void SessionsWindow::openSession()
{
    const auto *e = selected(); if (!e || !e->online || !m_open->isEnabled() || e->session.state == "archived") return;
    if (e->session.cmd == "dsh") { m_client.openNativeUi(e->host,e->session.name); return; }
    // `hgs a` resumes a saved session; a copied command starts nothing.
    if (!m_clipboard && e->session.state != "running") {
        if (!confirmLocalStart(*e)) return;
        e = selected(); if (!e->online || !m_open->isEnabled()) return;
    }
    emit sessionActivated(e->host, e->session.name,
        e->host.isEmpty() && e->session.attached && !e->session.clients.isEmpty() ? e->session.clients.first() : QString());
}

bool SessionsWindow::confirmLocalStart(const Entry &original)
{
    if (!LocalSessionHost::confirmLaunch(this, m_fleet.local(), original.host)) return false;
    const auto *e = selected();
    if (e && e->key == original.key && e->session.state == original.session.state && e->session.runId == original.session.runId
        && e->session.conversationId == original.session.conversationId && e->session.archiveId == original.session.archiveId) return true;
    showNotice(tr("The session changed. Open its actions again."), true); return false;
}

void SessionsWindow::changeSession()
{
    const auto *e = selected(); if (!e || !e->online || m_pending || !m_pause->isEnabled()) return;
    if (e->session.state != "running") {
        if (!confirmLocalStart(*e)) return;
        e = selected(); if (!e->online || m_pending || !m_pause->isEnabled()) return;
    }
    m_pending = true; m_restoreKey.clear();
    if (e->session.state == "archived") {
        m_restoreKey = e->host + '\n' + e->session.name;
        m_client.restoreArchive(e->host, e->session.name, e->session.archiveId);
    } else if (e->session.state == "running") m_client.pauseSession(e->host, e->session.name);
    else m_client.resumeSession(e->host, e->session.name);
    showNotice(tr("Updating session on %1…").arg(e->machine)); renderDetails();
}

void SessionsWindow::archiveSession()
{
    const auto *e = selected();
    if (!e || !e->online || m_pending || !m_renameKey.isEmpty() || (e->session.state != "paused" && e->session.state != "stopped")) return;
    const QString host = e->host, name = e->session.name;
    ++m_worktreeCleanupRevision; m_worktreeCleanup->hide();
    const bool inspected = m_details.value("git_worktree").toBool() && !m_details.value("git_root").toString().isEmpty();
    m_archivedWorktree = {host,name,inspected ? m_details.value("git_root").toString() : e->session.gitWorktree ? e->session.gitRoot : QString(),
        inspected ? m_details.value("git_common_dir").toString() : e->session.gitWorktree ? e->session.gitCommonDir : QString()};
    m_pending = true; m_restoreKey.clear(); m_client.archiveSession(host, name);
    showNotice(tr("Moving session to archive…")); renderDetails();
}

void SessionsWindow::offerWorktreeCleanup()
{
    if (m_archivedWorktree.size() != 4 || m_archivedWorktree[2].isEmpty()) return;
    const auto host = m_archivedWorktree[0], name = m_archivedWorktree[1], root = m_archivedWorktree[2];
    // Another running, paused or saved session keeps the checkout in use.
    for (const auto &entry : m_entries)
        if (entry.host == host && entry.session.name != name && entry.session.state != "archived" && entry.session.gitRoot == root) return;
    m_worktreeCleanup->setToolTip(tr("Review %1 for removal. Nothing is removed without your confirmation.").arg(root));
    m_worktreeCleanup->show();
    m_worktreeCleanup->setText(tr("Review remaining worktree…")); ++m_worktreeCleanupRevision;
}

void SessionsWindow::cleanUpArchivedWorktree()
{
    if (m_archivedWorktree.size() != 4) return;
    const auto offer = m_archivedWorktree; m_archivedWorktree.clear(); ++m_worktreeCleanupRevision; m_worktreeCleanup->hide();
    // The common directory still answers after the worktree folder is gone.
    WorktreeBatchCleanupDialog dialog(&m_client, {QJsonObject{{"host",offer[0]}, {"machine",offer[0].isEmpty() ? m_fleet.local().host : offer[0]},
        {"common_dir",offer[3].isEmpty() ? offer[2] : offer[3]}, {"row",QJsonObject{{"path",offer[2]}, {"kind","linked"}}}}}, this);
    dialog.exec();
    if (dialog.changed()) { emit refreshRequested(); updateWorktrees(); }
}

void SessionsWindow::forkSession()
{
    const auto *entry = selected(); if (!entry || !m_forkAction->isEnabled() || m_pending) return;
    const Entry original = *entry; const QString conversation = m_details.value("conversation_id").toString();
    const QString run = m_details.value("run_id").toString(original.session.runId);
    const QString prefix = original.session.name.section('/', 0, 1) + '/';
    const auto occupied = [&](const QString &tag) {
        return std::any_of(m_entries.cbegin(), m_entries.cend(), [&](const Entry &e) {
            return e.host == original.host && e.session.name == prefix + tag;
        });
    };
    QString base = original.session.tag.isEmpty() ? QStringLiteral("fork") : original.session.tag + "-fork";
    QString tag = base; for (int i = 2; occupied(tag); ++i) tag = base + '-' + QString::number(i);
    QDialog dialog(this); dialog.setObjectName("forkSessionDialog"); dialog.setWindowTitle(tr("Fork session")); dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(24,22,24,22); layout->setSpacing(12);
    auto *heading = label(tr("Fork session"), "heading"); layout->addWidget(heading);
    auto *description = label(tr("Create an independent conversation from this saved context.\nWorking folder: %1").arg(selectedDirectory()), "muted");
    description->setWordWrap(true); layout->addWidget(description);
    auto *name = new QLineEdit(tag); name->setObjectName("forkSessionName"); name->setAccessibleName(tr("New session name")); layout->addWidget(name);
    auto *error = label({}, "hint"); error->setWordWrap(true); layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); auto *create = buttons->button(QDialogButtonBox::Ok); create->setText(tr("Fork session")); layout->addWidget(buttons);
    const auto validate = [&] { QString problem = SessionTag::problem(name->text()); if (problem.isEmpty() && occupied(name->text())) problem = tr("This session name is already in use."); error->setText(problem); create->setEnabled(problem.isEmpty()); };
    connect(name, &QLineEdit::textChanged, &dialog, validate); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (LocalSessionHost::confirmLaunch(&dialog, m_fleet.local(), original.host)) dialog.accept();
    });
    validate(); name->selectAll(); name->setFocus();
    if (dialog.exec() != QDialog::Accepted || !SessionTag::problem(name->text()).isEmpty() || occupied(name->text())) return;
    if (!selectSessionEntry(original) || !m_forkAction->isEnabled()) return;
    m_forkKey = original.host + '\n' + prefix + name->text(); m_forkGroup = m_organization.groupFor(original.identity);
    if (const auto *group = m_organization.group(m_forkGroup)) {
        const int index = group->sessions.indexOf(original.identity); if (index >= 0 && index + 1 < group->sessions.size()) m_forkBefore = group->sessions[index + 1];
    }
    m_pending = true; m_restoreKey.clear(); m_renameKey.clear();
    showNotice(tr("Forking session on %1…").arg(original.machine)); renderDetails();
    m_client.forkSession(original.host, original.session.name, name->text(), original.session.archiveId, run, conversation);
}

void SessionsWindow::renameSession()
{
    const auto *entry = selected();
    if (!entry || !entry->online || m_pending || !m_renameAction->isEnabled()) return;
    // Fleet polling continues while the dialog is open, so keep value copies.
    const Entry original = *entry;
    const bool archived = original.session.state == "archived";
    const QString prefix = original.session.name.section('/', 0, 1) + '/';
    const QString oldTag = original.session.name.section('/', 2);
    QDialog dialog(this); dialog.setObjectName("renameSessionDialog"); dialog.setWindowTitle(tr("Rename session"));
    dialog.setMinimumWidth(430); dialog.setModal(true);
    auto *layout = new QVBoxLayout(&dialog); layout->setContentsMargins(24, 22, 24, 22); layout->setSpacing(14);
    auto *heading = label(tr("Rename session"), "renameHeading");
    heading->setStyleSheet("font-size:20px; font-weight:600"); layout->addWidget(heading);
    auto *context = label(original.machine + QStringLiteral("  /  ") + prefix.chopped(1), "muted");
    context->setWordWrap(true); layout->addWidget(context);
    auto *caption = label(tr("Session name"), "renameCaption"); layout->addWidget(caption);
    auto *name = new QLineEdit(oldTag, &dialog); name->setObjectName("renameSessionName");
    name->setAccessibleName(tr("Session name")); name->setPlaceholderText(tr("e.g. release planning"));
    caption->setBuddy(name); layout->addWidget(name);
    auto *preview = label({}, "renameFullName"); preview->setWordWrap(true);
    preview->setStyleSheet(QString("color:%1; font-size:12px").arg(m_muted)); layout->addWidget(preview);
    auto *error = label({}, "renameError"); error->setWordWrap(true);
    error->setStyleSheet(QString("color:%1").arg(m_dark ? "#ff9298" : "#b52b3d")); layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->button(QDialogButtonBox::Save); save->setText(tr("Rename")); save->setObjectName("primary");
    save->style()->unpolish(save); save->style()->polish(save);
    save->setDefault(true); layout->addWidget(buttons);
    const auto problem = [this, original, archived, prefix](const QString &tag) {
        const QString invalid = SessionTag::problem(tag); if (!invalid.isEmpty()) return invalid;
        if (!archived) for (const auto &other : m_entries)
            if (other.host == original.host && other.key != original.key && other.session.state != "archived" && other.session.name == prefix + tag)
                return tr("This name is already in use on %1.").arg(original.machine);
        return QString();
    };
    const auto validate = [&]() {
        const QString invalid = problem(name->text());
        error->setText(invalid); error->setVisible(!invalid.isEmpty());
        preview->setText(prefix + name->text());
        save->setEnabled(invalid.isEmpty() && name->text() != oldTag);
    };
    connect(name, &QLineEdit::textChanged, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    validate(); name->selectAll(); name->setFocus();
    if (dialog.exec() != QDialog::Accepted || name->text() == oldTag || !problem(name->text()).isEmpty()) return;
    if (!selectSessionEntry(original) || !selected()->online || m_pending || !m_renameAction->isEnabled()) return;
    const QString newName = prefix + name->text();
    m_pending = true; m_restoreKey.clear(); m_renameArchived = archived;
    m_renameKey = original.host + '\n' + newName + (archived ? '\n' + original.session.archiveId : QString());
    showNotice(tr("Renaming session on %1…").arg(original.machine)); renderDetails();
    m_client.renameSession(original.host, original.session.name, newName, original.session.archiveId);
}

void SessionsWindow::terminateSession()
{
    const auto *e = selected(); if (!e || !e->online || isTerminating(*e) || m_pending || !m_renameKey.isEmpty()
        || !NativeSessionCapabilities::action(e->session,e->session.state=="running"?"terminate":"forget",m_details)) return;
    const Entry original = *e;
    const QString host = e->host, name = e->session.name, archiveId = e->session.archiveId;
    const bool live = e->session.state == "running", archived = e->session.state == "archived";
    const bool keepArchive = live && e->session.tracked && QStringList{"codex","claude","kimi"}.contains(e->session.cmd);
    QMessageBox confirm(QMessageBox::Warning, live ? tr("Terminate session?") : archived ? tr("Forget archived session?") : tr("Forget saved session?"),
        live ? (keepArchive ? tr("Stop %1 and move its recorded conversation to Archive? Running processes will stop; the conversation history is kept.").arg(name) : tr("Terminate %1? Its running processes will stop. No HGS archive is available for this session; any native conversation history is kept.").arg(name))
             : archived ? tr("Remove this archived instance of %1 from HGS? Other sessions with this name and the agent’s native conversation history are kept.").arg(name)
             : tr("Remove %1 from HGS? The agent’s native conversation history is kept.").arg(name), QMessageBox::Yes | QMessageBox::Cancel, this);
    confirm.setObjectName("terminateSessionConfirm"); confirm.setTextFormat(Qt::PlainText);
    confirm.setInformativeText(tr("Computer: %1").arg(original.machine));
    confirm.button(QMessageBox::Yes)->setText(live ? (keepArchive ? tr("Stop and archive") : tr("Terminate session"))
        : archived ? tr("Forget archived session") : tr("Forget saved session"));
    confirm.setDefaultButton(QMessageBox::Cancel); confirm.setEscapeButton(QMessageBox::Cancel);
    if (confirm.exec() != QMessageBox::Yes || !selectSessionEntry(original)) return;
    const auto *current = selected();
    if (!current || !current->online || isTerminating(*current) || m_pending || !m_renameKey.isEmpty()) return;
    if ((current->session.state == "running") != live) {
        showNotice(tr("The session state changed. Open its actions again."), true); return;
    }
    m_pending = true; m_restoreKey.clear();
    if(live){
        ++m_worktreeCleanupRevision; m_worktreeCleanup->hide();
        const bool inspected=m_details.value("git_worktree").toBool() && !m_details.value("git_root").toString().isEmpty();
        m_archivedWorktree = keepArchive ? QStringList{host,name,inspected ? m_details.value("git_root").toString() : original.session.gitWorktree ? original.session.gitRoot : QString(),
            inspected ? m_details.value("git_common_dir").toString() : original.session.gitWorktree ? original.session.gitCommonDir : QString()} : QStringList{};
        m_terminationFinished.remove(original.key);m_terminating.insert(original.key,original);m_terminationCommand=original.key;
        showNotice(tr("Terminating %1…").arg(name));rebuild();
    }
    if (archived) m_client.forgetArchive(host, name, archiveId);
    else if(keepArchive)m_client.terminateSession(host,name,original.session.runId);
    else m_client.killSession(host, name);
    renderDetails();
}

void SessionsWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event); rebuild(); if (m_dashboardStale) updateDashboard();
    m_timer.start(); m_readTimer.start(); emit refreshRequested(); inspect();
}
bool SessionsWindow::eventFilter(QObject *watched, QEvent *event)
{
    if(m_inspector && watched==m_inspector->tabBar()
        && (event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange || event->type()==QEvent::LayoutRequest || event->type()==QEvent::Resize)) {
        QTimer::singleShot(0,this,&SessionsWindow::updateInspectorMinimum);
    }

    if (m_children && watched == m_children->viewport() && event->type() == QEvent::Resize)
        QTimer::singleShot(0, this, &SessionsWindow::renderDetails);
    if ((watched == m_meta || watched == m_title) && (event->type() == QEvent::Resize || event->type() == QEvent::FontChange)) updateHeaderText();
    if (watched == m_sessions && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Menu || (key->key() == Qt::Key_F10 && key->modifiers().testFlag(Qt::ShiftModifier))) {
            if (const auto *item = m_sessions->currentItem())
                showSessionMenu(item->data(KeyRole).toString(), m_sessions->viewport()->mapToGlobal(m_sessions->visualItemRect(item).center()));
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            jumpToLatestActivity(m_sessions->currentItem()); return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
void SessionsWindow::hideEvent(QHideEvent *event) { m_readTimer.stop(); m_readCandidate.clear(); m_timer.stop(); m_terminal->disconnectSession(); QWidget::hideEvent(event); }
void SessionsWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings; settings.setValue("sessions/geometry", saveGeometry()); settings.setValue("workspace/splitter", m_splitter->saveState());
    settings.setValue("workspace/sessionsWidth", m_sessionDock->dockedWidth());
    QWidget::closeEvent(event);
}
void SessionsWindow::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && m_brand) m_brand->syncAnimation();
    if (event->type() == QEvent::ApplicationPaletteChange && m_activityView) applyTheme();
}

void SessionsWindow::updateWorktrees()
{
    if(!m_worktrees||!m_inspectorPanel||!m_inspector)return;
    const auto *entry=selected();
    m_worktrees->setContext(entry?entry->host:QString(),entry?(m_subagentId.isEmpty()?m_details.value("cwd").toString(entry->session.cwd):m_subagentCwd):QString(),m_fleet);
}
void SessionsWindow::filterFolder(const QString &host,const QString &path,bool checkout)
{
    m_projectFilters.clear(); m_folderFilterHost=host;m_folderFilterPath=path;m_folderFilterCheckout=checkout;
    if(path.isEmpty()){m_filter="all";m_hostFilters.clear();m_search->clear();}
    showSessionList();
}
void SessionsWindow::openRelatedSession(const QString &host,const QString &name,const QString &archive)
{
    m_folderFilterPath.clear();m_filter=archive.isEmpty()?"all":"archived";m_hostFilters.clear(); m_projectFilters.clear();m_search->clear();showSessionList();
    const auto key=host+'\n'+name+(archive.isEmpty()?QString():'\n'+archive);revealSession(key);
    for(int i=0;i<m_sessions->count();++i)if(m_sessions->item(i)->data(KeyRole).toString()==key){m_sessions->setCurrentRow(i);m_sessions->scrollToItem(m_sessions->item(i));return;}
}
