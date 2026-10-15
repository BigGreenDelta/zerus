#include "WorktreesDialog.h"
#include "DialogButtons.h"
#include "WorktreeBatchCleanupDialog.h"
#include "WorktreeInspectionPanel.h"
#include "WorktreeReviewUi.h"
#include "WorkspaceIcons.h"
#include <QCheckBox>
#include <QLineEdit>
#include <QSplitter>
#include <QTreeWidgetItemIterator>
#include <QTimer>
#include <QApplication>
#include <algorithm>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QMenu>
#include <QWidgetAction>
#include <QStyledItemDelegate>

namespace {
// Keep the native selection/check box, but give folder and branch different
// visual roles without creating a widget for every catalog row.
class WorktreeNameDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (index.parent().isValid())
            size.setHeight(qMax(size.height(), option.fontMetrics.lineSpacing() * 2 + 8));
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (!index.parent().isValid()) { QStyledItemDelegate::paint(painter, option, index); return; }
        QStyleOptionViewItem base(option); initStyleOption(&base, index);
        const auto *style = option.widget ? option.widget->style() : QApplication::style();
        const auto area = style->subElementRect(QStyle::SE_ItemViewItemText, &base, option.widget).adjusted(4, 0, -4, 0);
        base.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &base, painter, option.widget);
        const auto row = index.data(Qt::UserRole + 1).toJsonObject();
        const auto branch = index.data(Qt::UserRole + 4).toJsonObject()["branch"].toString();
        const auto textColor = option.palette.color(option.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text);
        const int line = option.fontMetrics.lineSpacing(), iconSize = qMin(20, line);
        const int top = area.center().y() - line - 1;
        painter->save(); painter->setClipRect(option.rect);
        auto titleFont = option.font; titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont); painter->setPen(textColor);
        painter->drawText(QRect(area.left(), top, area.width(), line), Qt::AlignVCenter,
            QFontMetrics(titleFont).elidedText(QFileInfo(row["path"].toString()).fileName(), Qt::ElideMiddle, area.width()));
        painter->setFont(option.font);
        painter->setOpacity(option.state & QStyle::State_Selected ? 1.0 : 0.72);
        workspaceIcon(row["kind"] == "folder" || row["kind"] == "bare" ? "folder" : "git-branch", textColor)
            .paint(painter, QRect(area.left(), top + line + 2, iconSize, iconSize));
        const auto label = row["kind"] == "folder" ? QObject::tr("Ordinary folder")
            : row["kind"] == "bare" ? QObject::tr("Bare repository")
            : branch.isEmpty() ? QObject::tr("Detached HEAD") : branch;
        painter->drawText(QRect(area.left() + iconSize + 5, top + line + 2, qMax(0, area.width() - iconSize - 5), line),
            Qt::AlignVCenter, option.fontMetrics.elidedText(label, Qt::ElideMiddle, qMax(0, area.width() - iconSize - 5)));
        painter->restore();
    }
};
}

QString WorktreesDialog::key(const QString &host, const QString &path)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray{host, path}).toJson(QJsonDocument::Compact));
}

WorktreesDialog::WorktreesDialog(HgsClient *client, const SessionOrganization &projects,
                               const FleetState &fleet, bool allProjects, QWidget *parent)
    : QDialog(parent), m_client(client)
{
    DialogButtons::install();
    setObjectName("worktreesDialog"); setWindowTitle(tr("Worktrees"));
    setMinimumSize(820, 580); resize(1180, 760);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(14, 12, 14, 12); layout->setSpacing(8);
    auto *toolbar = new QWidget; toolbar->setObjectName("worktreeToolbar");
    toolbar->setStyleSheet("QLineEdit, QComboBox { padding:5px 8px; min-height:18px; } QPushButton { padding:5px 10px; min-height:18px; }");
    auto *searchRow = new QHBoxLayout(toolbar); searchRow->setContentsMargins(0,0,0,0); searchRow->setSpacing(8);
    m_search = new QLineEdit; m_search->setObjectName("worktreeSearch"); m_search->setPlaceholderText(tr("Search worktrees…")); m_search->setAccessibleName(tr("Search folder, branch, project or session")); searchRow->addWidget(m_search, 1);
    m_filters = new QPushButton(tr("Filters")); m_filters->setObjectName("worktreeFilters"); m_filters->setAutoDefault(false);
    auto *filterMenu = new QMenu(m_filters); filterMenu->setObjectName("worktreeFiltersMenu");
    auto *filterWidget = new QWidget; auto *filters = new QFormLayout(filterWidget);
    filters->setContentsMargins(12,10,12,10); filters->setSpacing(8);
    m_project = new QComboBox; m_project->setObjectName("worktreeProjectFilter"); m_project->setAccessibleName(tr("Project"));
    m_computer = new QComboBox; m_computer->setObjectName("worktreeComputerFilter"); m_computer->setAccessibleName(tr("Computer"));
    for (auto *filter : {m_project, m_computer}) { filter->setMinimumContentsLength(10); filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); }
    filters->addRow(tr("Project"), m_project); filters->addRow(tr("Computer"), m_computer);
    m_category = new QComboBox; m_category->setObjectName("worktreeStateFilter"); m_category->addItem(tr("All states"), -1);
    for (int group = 0; group < 4; ++group) m_category->addItem(WorktreeReviewUi::categoryName(group), group);
    m_category->setAccessibleName(tr("Worktree state")); filters->addRow(tr("State"), m_category);
    m_showMain = new QCheckBox(tr("Show main checkouts and ordinary folders")); m_showMain->setObjectName("showMainWorktreeFolders"); filters->addRow(m_showMain);
    auto *filterAction = new QWidgetAction(filterMenu); filterAction->setDefaultWidget(filterWidget); filterMenu->addAction(filterAction);
    m_filters->setMenu(filterMenu); searchRow->addWidget(m_filters);
    m_sort = new QComboBox; m_sort->setObjectName("worktreeSort"); m_sort->setAccessibleName(tr("Sort worktrees"));
    m_sort->addItems({tr("Largest first"), tr("Oldest activity first"), tr("Folder name")});
    m_sort->setMinimumContentsLength(8); m_sort->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); m_sort->setMaximumWidth(210); searchRow->addWidget(m_sort);
    m_refresh = new QPushButton; m_refresh->setObjectName("refreshWorktreeOverview"); configureWorkspaceIconButton(m_refresh, "refresh", tr("Refresh"), palette().color(QPalette::Window).lightness() < 128); searchRow->addWidget(m_refresh);
    m_refresh->setMinimumSize(0,0); m_refresh->setMaximumSize(QWIDGETSIZE_MAX,QWIDGETSIZE_MAX);
    m_refresh->setStyleSheet("QPushButton { padding:5px 10px; min-height:18px; }");
    m_refresh->setText(tr("Refresh"));
    layout->addWidget(toolbar);
    m_status = new QLabel; m_status->setObjectName("worktreeOverviewStatus"); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); layout->addWidget(m_status);
    auto *split = new QSplitter; split->setObjectName("worktreeWorkspaceSplit"); split->setChildrenCollapsible(false);
    m_tree = new QTreeWidget; m_tree->setObjectName("worktreeOverviewList"); m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({tr("Worktree / branch"), tr("What remains"), tr("Size"), tr("Computer")});
    m_tree->setItemDelegateForColumn(0, new WorktreeNameDelegate(m_tree));
    m_tree->setTextElideMode(Qt::ElideMiddle); m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setMinimumWidth(360); m_tree->header()->setStretchLastSection(false); m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::Stretch); m_tree->setColumnWidth(0, 230);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Interactive); m_tree->setColumnWidth(2,110); m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    split->addWidget(m_tree); m_details = new WorktreeInspectionPanel(client); m_details->setMinimumWidth(300); split->addWidget(m_details); split->setSizes({650, 450}); layout->addWidget(split, 1);
    m_details->sessionRequested = [this](const QString &host, const QString &name) { accept(); if (sessionRequested) sessionRequested(host, name); };
    auto *buttons = new QDialogButtonBox;
    m_copy = buttons->addButton(tr("Copy path"), QDialogButtonBox::ActionRole); m_copy->setObjectName("copyOverviewWorktreePath");
    m_cleanup = buttons->addButton(tr("Select candidates"), QDialogButtonBox::ActionRole); m_cleanup->setObjectName("selectCleanupCandidates");
    m_remove = buttons->addButton(tr("Review cleanup…"), QDialogButtonBox::ActionRole); m_remove->setObjectName("reviewSelectedWorktrees");
    auto *close = buttons->addButton(QDialogButtonBox::Close);
    for (auto *button : {m_refresh, m_copy, m_cleanup, m_remove, close}) button->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_refresh, &QPushButton::clicked, this, &WorktreesDialog::refresh);
    for (auto *filter : {m_project, m_computer, m_category, m_sort}) connect(filter, &QComboBox::currentIndexChanged, this, [this] { render(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { render(); });
    connect(m_showMain, &QCheckBox::toggled, this, [this] { render(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] { showDetails(); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (column != 0 || !item->parent()) return;
        const auto identity = item->data(0, Qt::UserRole + 2).toString();
        if (item->checkState(0) == Qt::Checked) m_checked[identity] = item->data(0, Qt::UserRole + 4).toJsonObject()["fingerprint"].toString();
        else m_checked.remove(identity); updateActions();
    });
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(selectedRow()["path"].toString()); });
    connect(m_cleanup, &QPushButton::clicked, this, [this] {
        for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
            auto *item = *it; const auto review = item->data(0, Qt::UserRole + 4).toJsonObject();
            if (item->parent() && (item->flags() & Qt::ItemIsUserCheckable)) item->setCheckState(0, WorktreeReviewUi::category(review) == 0 ? Qt::Checked : Qt::Unchecked);
        }
    });
    connect(m_remove, &QPushButton::clicked, this, &WorktreesDialog::cleanUp);
    auto *expiry = new QTimer(this); expiry->setObjectName("worktreeCacheTimer"); expiry->setInterval(15000);
    connect(expiry, &QTimer::timeout, this, [this] {
        bool due=QDateTime::currentSecsSinceEpoch()>=m_autoRefreshAt, connected=false;
        for(const auto &source:m_sources) {
            connected |= online(source);
            const auto age=QDateTime::currentSecsSinceEpoch()-qint64(source.catalog["received_at"].toDouble(source.catalog["sampled_at"].toDouble()));
            due |= online(source)&&source.fresh&&(age<0||age>=3600);
        }
        if(due&&connected) refresh(); else render(); // Expire cleanup selection without discarding browsing data.
    }); expiry->start();
    connect(m_client,&HgsClient::worktreeCacheInvalidated,this,[this](const QString &host,const QString &common) {
        for(auto &source:m_sources) if(source.host==host&&source.catalog["common_dir"]==common) {
            source.attempted=false; source.fresh=false;
            const auto repository=key(host,common);const auto display=m_reviews.value(repository);
            invalidateReview(repository);if(!display.isEmpty())m_reviews[repository]=display;
        }
        m_checked.clear();
        QTimer::singleShot(0,this,[this]{pump();render();});
    });
    connect(m_client, &HgsClient::worktreesReady, this,
        [this](quint64 id, const QString &host, const QString &path, const QJsonObject &data) {
            const auto request = m_requests.constFind(id);
            if (request == m_requests.cend() || request->host != host || request->path != path) return;
            const auto scope = request.value(); m_requests.remove(id);
            auto source = m_sources.find(scope.source);
            if (scope.generation == m_generation && source != m_sources.end()) {
                source->catalog = data; source->error.clear(); source->fresh = !data["stale"].toBool();
            }
            render(); pump();
        });
    connect(m_client, &HgsClient::worktreesFailed, this,
        [this](quint64 id, const QString &host, const QString &path, const QString &error) {
            const auto request = m_requests.constFind(id);
            if (request == m_requests.cend() || request->host != host || request->path != path) return;
            const auto scope = request.value(); m_requests.remove(id);
            auto source = m_sources.find(scope.source);
            if (scope.generation == m_generation && source != m_sources.end()) { source->error = error; source->fresh = false; }
            render(); pump();
        });
    connect(m_client, &HgsClient::worktreeReviewReady, this,
        [this](quint64 id, const QString &host, const QString &path, const QJsonObject &data) {
            if (id != m_reviewRequest || key(host, path) != m_reviewKey) return;
            m_reviewRequest = 0;
            if (m_reviewGeneration == m_generation) {
                if (data["state"] == "ok" && key(host, data["common_dir"].toString()) == m_reviewKey) {
                    auto merged = m_reviewWorktree.isEmpty() ? data : m_reviews.value(m_reviewKey);
                    QJsonArray rows;
                    if (!m_reviewWorktree.isEmpty()) for (const auto &value : merged["worktrees"].toArray()) if (value.toObject()["path"] != m_reviewWorktree) rows.append(value);
                    for (const auto &value : data["worktrees"].toArray()) {
                        auto row = value.toObject(); if (!m_reviewWorktree.isEmpty() && row["path"] != m_reviewWorktree) continue;
                        row["sampled_at"] = data["sampled_at"]; rows.append(row);
                    }
                    merged["worktrees"] = rows; merged["sampled_at"] = data["sampled_at"]; m_reviews[m_reviewKey] = merged; m_reviewErrors.remove(m_reviewKey);
                } else m_reviewErrors[m_reviewKey] = data["detail"].toString(tr("Cleanup review is unavailable."));
            }
            render();
        });
    connect(m_client, &HgsClient::worktreeReviewFailed, this,
        [this](quint64 id, const QString &host, const QString &path, const QString &error) {
            if (id != m_reviewRequest || key(host, path) != m_reviewKey) return;
            m_reviewRequest = 0;
            if (m_reviewGeneration == m_generation) m_reviewErrors[m_reviewKey] = error;
            render();
        });
    setContext(projects, fleet, allProjects);
}

const BoxState *WorktreesDialog::box(const Source &source) const
{
    return source.host.isEmpty() ? &m_fleet.local() : m_fleet.peer(source.host);
}

bool WorktreesDialog::online(const Source &source) const
{
    const auto *machine = box(source);
    return !source.machine.isEmpty() && !m_fleet.local().host.isEmpty() && machine && machine->ok
        && (source.host.isEmpty() || QDateTime::currentMSecsSinceEpoch() - m_fleet.peerPolledAt(source.host) < FleetState::kPeerStaleMs);
}

void WorktreesDialog::setContext(const SessionOrganization &projects, const FleetState &fleet, bool allProjects)
{
    QMap<QString, Source> sources; QJsonArray scope;
    QMap<QString, QString> projectNames; QSet<QString> computers;
    for (const auto &project : projects.groups()) {
        if (!project.accessible && !allProjects) continue;
        projectNames[project.id] = project.name;
        for (const auto &folder : project.folders) {
            if (folder.path.isEmpty() || folder.machine.isEmpty()) continue;
            const auto host = folder.machine == fleet.local().host ? QString() : folder.machine;
            const auto address = key(host, folder.path);
            auto &source = sources[address]; source.host = host; source.machine = folder.machine; source.path = folder.path;
            source.projectIds.append(project.id); source.projectNames.append(project.name); computers.insert(folder.machine);
            scope.append(QJsonArray{project.id, project.name, host, folder.machine, folder.path});
        }
    }
    const auto context = QJsonDocument(QJsonArray{fleet.local().host, scope}).toJson(QJsonDocument::Compact);
    if (context != m_context) {
        m_context = context; ++m_generation;m_autoRefreshAt=QDateTime::currentSecsSinceEpoch()+3600;
        m_sources = sources; m_checked.clear(); m_reviews.clear(); m_reviewErrors.clear(); m_reviewAttempted.clear();
        for (auto &source : m_sources) {
            source.catalog = m_client->worktreeSnapshot(source.host, source.path);
            const auto age=QDateTime::currentSecsSinceEpoch()-qint64(source.catalog["received_at"].toDouble(source.catalog["sampled_at"].toDouble()));
            source.fresh=!source.catalog.isEmpty()&&!source.catalog["stale"].toBool()&&age>=0&&age<3600;
            source.attempted=source.fresh;
            const auto common=source.catalog["common_dir"].toString(), repository=key(source.host,common);
            const auto review=m_client->worktreeReview(source.host,common,3600);
            if(source.fresh&&!review.isEmpty()) {
                m_reviews[repository]=review;m_reviewAttempted.insert(repository);
                for(const auto &path:review["targeted_paths"].toArray())m_reviewAttempted.insert(key(repository,path.toString()));
            }
        }
        const auto project = m_project->currentData().toString(), computer = m_computer->currentData().toString();
        const QSignalBlocker projectBlock(m_project), computerBlock(m_computer);
        m_project->clear(); m_project->addItem(tr("All projects"), QString());
        for (auto it = projectNames.cbegin(); it != projectNames.cend(); ++it) m_project->addItem(it.value(), it.key());
        m_project->setCurrentIndex(qMax(0, m_project->findData(project)));
        m_computer->clear(); m_computer->addItem(tr("All computers"), QString());
        auto ordered = computers.values(); ordered.sort(Qt::CaseInsensitive);
        for (const auto &machine : ordered) m_computer->addItem(machine, machine);
        m_computer->setCurrentIndex(qMax(0, m_computer->findData(computer)));
    } else {
        for (auto &source : m_sources) if (!online(source)) {
            source.attempted = false; source.fresh = false;
            const auto repository = key(source.host, source.catalog["common_dir"].toString());
            invalidateReview(repository); m_reviewErrors.remove(repository);
        }
    }
    m_fleet = fleet; render(); pump();
}

void WorktreesDialog::refresh()
{
    if (!m_requests.isEmpty() || m_reviewRequest) return;
    m_autoRefreshAt=QDateTime::currentSecsSinceEpoch()+3600;
    ++m_generation; m_checked.clear(); m_reviewErrors.clear(); m_reviewAttempted.clear();
    for (auto &source : m_sources) {
        m_client->invalidateWorktreeCache(source.host,source.catalog["common_dir"].toString());
        source.attempted = false; source.fresh = false; source.error.clear();
    }
    pump(); render();
}

void WorktreesDialog::pump()
{
    // Two catalog subprocesses at most, including obsolete requests whose
    // responses must be ignored after a project/identity change.
    for (auto it = m_sources.begin(); it != m_sources.end() && m_requests.size() < 2; ++it) {
        auto &source = it.value();
        if (source.attempted || !online(source)) continue;
        source.attempted = true;
        const auto id = m_client->requestWorktrees(source.host, source.path, true);
        m_requests[id] = {it.key(), source.host, source.path, m_generation};
    }
    showDetails();
}

WorktreesDialog::Source *WorktreesDialog::selectedSource()
{
    const auto *item = m_tree->currentItem();
    if (!item) return nullptr;
    const auto source = m_sources.find(item->data(0, Qt::UserRole).toString());
    return source == m_sources.end() ? nullptr : &source.value();
}

QJsonObject WorktreesDialog::selectedRow() const
{
    const auto *item = m_tree->currentItem();
    return item ? item->data(0, Qt::UserRole + 1).toJsonObject() : QJsonObject();
}

QJsonObject WorktreesDialog::reviewedRow(const Source &source, const QJsonObject &row) const
{
    // Cached verdicts describe the last check, including while offline or refreshing.
    // Selection and removal separately require a current online proof.
    const auto review = m_reviews.value(key(source.host, source.catalog["common_dir"].toString()));
    for (const auto &value : review["worktrees"].toArray()) if (value.toObject()["path"] == row["path"]) {
        const auto found = value.toObject(); const auto sampled = qint64(found["sampled_at"].toDouble(review["sampled_at"].toDouble()));
        if (sampled > 0 && QDateTime::currentSecsSinceEpoch() >= sampled && QDateTime::currentSecsSinceEpoch() - sampled < 3600) return found;
    }
    return {};
}

bool WorktreesDialog::freshProof(const Source &source,const QJsonObject &row) const
{
    if(!canClean(source)) return false;
    const auto sampled=qint64(row["sampled_at"].toDouble());
    const auto age=QDateTime::currentSecsSinceEpoch()-sampled;
    return sampled>0&&age>=0&&age<300;
}

QString WorktreesDialog::state(const Source &source, const QJsonObject &row) const
{
    if (!online(source)) return tr("Offline snapshot");
    if (!source.error.isEmpty()) return tr("Unavailable");
    if (!source.fresh || source.catalog["stale"].toBool()) return tr("Loading…");
    const auto catalogState = source.catalog["state"].toString();
    if (catalogState == "not_repo") return tr("Ordinary folder");
    if (catalogState != "ok") return tr("Unavailable");
    const auto review = reviewedRow(source, row);
    const auto verdict = review["verdict"].toString();
    if (verdict == "ready" || verdict == "review") return WorktreeReviewUi::categoryName(WorktreeReviewUi::category(review));
    if (verdict == "missing") return tr("Missing");
    if (verdict == "blocked") return tr("Blocked");
    if (row["kind"] == "main") return tr("Main checkout");
    if (row["kind"] == "bare") return tr("Bare repository");
    if (row["locked"].toBool()) return tr("Locked");
    if (!row["available"].toBool()) return tr("Missing");
    if (m_reviewErrors.contains(key(source.host, source.catalog["common_dir"].toString()))) return tr("Review unavailable");
    return tr("Not reviewed");
}

QStringList WorktreesDialog::sessions(const Source &source, const QJsonObject &row) const
{
    QStringList result;
    if (const auto *machine = box(source)) for (const auto &session : machine->sessions) {
        const auto path = row["kind"] == "folder" ? session.canonicalCwd : session.gitRoot;
        if (path == row["path"].toString()) result.append(session.name + " (" + session.state + ')');
    }
    return result;
}

bool WorktreesDialog::canClean(const Source &source) const
{
    if (!online(source) || !source.fresh || !source.error.isEmpty() || source.catalog["stale"].toBool()
        || source.catalog["state"] != "ok" || source.catalog["common_dir"].toString().isEmpty()) return false;
    for (const auto &value : source.catalog["worktrees"].toArray()) if (value.toObject()["kind"] == "linked") return true;
    return false;
}

void WorktreesDialog::render()
{
    const int filterCount = int(!m_project->currentData().toString().isEmpty()) + int(!m_computer->currentData().toString().isEmpty())
        + int(m_category->currentData().toInt() >= 0) + int(m_showMain->isChecked());
    m_filters->setText(filterCount ? tr("Filters (%1)").arg(filterCount) : tr("Filters"));
    m_filters->setToolTip(tr("%1 · %2 · %3").arg(m_project->currentText(), m_computer->currentText(), m_category->currentText()));
    const auto selected = m_tree->currentItem() ? m_tree->currentItem()->data(0, Qt::UserRole + 2).toString() : QString();
    const auto scroll = m_tree->verticalScrollBar()->value();
    QMap<QString, QStringList> repositories;
    QSet<QString> scopeMachines;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        const auto &source = it.value();
        if (!m_project->currentData().toString().isEmpty() && !source.projectIds.contains(m_project->currentData().toString())) continue;
        if (!m_computer->currentData().toString().isEmpty() && source.machine != m_computer->currentData().toString()) continue;
        scopeMachines.insert(source.machine);
        const auto common = source.catalog["common_dir"].toString(); repositories[key(source.host, common.isEmpty() ? source.path : common)].append(it.key());
    }
    m_tree->setColumnHidden(3,scopeMachines.size()==1);
    QJsonArray rendered; int linked = 0, checked = 0, candidates = 0; qint64 candidateBytes = 0; bool candidateComplete = true, partial = false;
    for (const auto &links : repositories) {
        auto sourceKey = links.first(); for (const auto &link : links) if (m_sources[link].fresh && m_sources[link].error.isEmpty()) { sourceKey = link; break; }
        const auto &source = m_sources[sourceKey]; QStringList projects;
        for (const auto &link : links) projects.append(m_sources[link].projectNames); projects.removeDuplicates();
        auto rows = source.catalog["worktrees"].toArray(); if (rows.isEmpty()) rows.append(QJsonObject{{"path", source.path}, {"kind", "folder"}});
        partial |= source.catalog["partial"].toBool();
        for (const auto &value : rows) {
            const auto row = value.toObject(); const auto path = row["path"].toString(); auto reviewed = reviewedRow(source, row);
            const bool valid = !reviewed.isEmpty(); QJsonObject detail = valid ? reviewed : row;
            int group = WorktreeReviewUi::category(detail, valid);
            if (row["kind"] == "folder" && source.catalog["state"] != "not_repo") group = 3;
            if (row["kind"] == "linked") { ++linked; if (valid && group != 3) ++checked; }
            if (group == 4 && !m_showMain->isChecked()) continue;
            if (m_category->currentData().toInt() >= 0 && group != m_category->currentData().toInt()) continue;
            const auto related = sessions(source, row);
            const auto haystack = path + ' ' + row["branch"].toString() + ' ' + projects.join(' ') + ' ' + related.join(' ');
            if (!haystack.contains(m_search->text().trimmed(), Qt::CaseInsensitive)) continue;
            if (group == 0) { ++candidates; candidateBytes += qint64(detail["disk_bytes"].toDouble()); candidateComplete &= detail["verdict"] == "missing" || detail["disk_complete"] == true; }
            const auto summary = (valid ? WorktreeReviewUi::summary(detail) : state(source, row))
                + (valid && !online(source) ? tr(" · Offline snapshot") :
                   valid && WorktreeReviewUi::removable(detail) && !freshProof(source,detail) ? tr(" · Refresh to select") : QString());
            rendered.append(QJsonObject{{"source", sourceKey}, {"row", row}, {"review", detail}, {"identity", key(source.host, path)}, {"group", group},
                {"selectable", valid && freshProof(source,detail) && WorktreeReviewUi::removable(detail)}, {"projects", QJsonArray::fromStringList(projects)},
                {"columns", QJsonArray{QFileInfo(path).fileName() + '\n' + detail["branch"].toString(), summary, WorktreeReviewUi::size(detail), source.machine}}});
        }
    }
    QList<QJsonValue> ordered; for (const auto &entry : rendered) ordered << entry;
    std::stable_sort(ordered.begin(), ordered.end(), [this](const QJsonValue &a, const QJsonValue &b) {
        const auto left = a.toObject()["review"].toObject(), right = b.toObject()["review"].toObject();
        if (m_sort->currentIndex() == 0 && left["disk_bytes"] != right["disk_bytes"]) return left["disk_bytes"].toDouble(-1) > right["disk_bytes"].toDouble(-1);
        if (m_sort->currentIndex() == 1 && left["last_activity"] != right["last_activity"]) return left["last_activity"].toDouble(1e20) < right["last_activity"].toDouble(1e20);
        return QString::localeAwareCompare(left["path"].toString(), right["path"].toString()) < 0;
    });
    rendered = {}; for (const auto &entry : ordered) rendered.append(entry);
    const auto fingerprint = QJsonDocument(rendered).toJson(QJsonDocument::Compact);
    if (fingerprint != m_rendered) {
        m_rendered = fingerprint; const QSignalBlocker blocker(m_tree); m_tree->clear();
        for (int group = 0; group < 5; ++group) {
            QTreeWidgetItem *heading = nullptr; int count = 0;
            for (const auto &value : rendered) {
                const auto entry = value.toObject(); if (entry["group"].toInt() != group) continue;
                if (!heading) { heading = new QTreeWidgetItem(m_tree); heading->setFlags(Qt::ItemIsEnabled); auto font = heading->font(0); font.setBold(true); heading->setFont(0, font); heading->setExpanded(true); heading->setFirstColumnSpanned(true); }
                ++count; QStringList columns; for (const auto &column : entry["columns"].toArray()) columns << column.toString();
                auto *item = new QTreeWidgetItem(heading, columns);
                item->setData(0, Qt::UserRole, entry["source"].toString()); item->setData(0, Qt::UserRole + 1, entry["row"].toObject());
                item->setData(0, Qt::UserRole + 2, entry["identity"].toString()); item->setData(0, Qt::UserRole + 3, entry["projects"].toArray()); item->setData(0, Qt::UserRole + 4, entry["review"].toObject());
                const auto identity = entry["identity"].toString(), reviewFingerprint = entry["review"].toObject()["fingerprint"].toString();
                if (entry["selectable"].toBool()) {
                    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
                    item->setCheckState(0, m_checked.value(identity) == reviewFingerprint && !reviewFingerprint.isEmpty() ? Qt::Checked : Qt::Unchecked);
                } else { item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable); m_checked.remove(identity); }
                const auto branch = entry["review"].toObject()["branch"].toString();
                const auto kind = entry["row"].toObject()["kind"].toString();
                const auto description = tr("Folder: %1\n%2").arg(entry["row"].toObject()["path"].toString(),
                    kind == "folder" || kind == "bare" ? columns[1] : tr("Branch: %1\n%2").arg(branch.isEmpty() ? tr("Detached HEAD") : branch, columns[1]));
                item->setData(0, Qt::AccessibleDescriptionRole, description);
                for (int column = 0; column < 4; ++column) item->setToolTip(column, description);
                if (identity == selected) m_tree->setCurrentItem(item);
            }
            if (heading) { heading->setText(0, tr("%1 (%2)").arg(WorktreeReviewUi::categoryName(group)).arg(count)); heading->setExpanded(true); }
        }
        if (!m_tree->currentItem() && m_tree->topLevelItemCount()) m_tree->setCurrentItem(m_tree->topLevelItem(0)->child(0));
        m_tree->verticalScrollBar()->setValue(scroll);
    }
    qint64 oldest=0;
    for(const auto &source:m_sources) {
        const auto at=qint64(source.catalog["received_at"].toDouble(source.catalog["sampled_at"].toDouble()));
        if(at>0&&(oldest==0||at<oldest))oldest=at;
    }
    const bool checking = !m_requests.isEmpty() || m_reviewRequest;
    m_status->setText(m_sources.isEmpty() ? tr("Add a project folder to see its worktrees.") :
        tr("%1 of %2 worktrees checked · %3 repositories%4").arg(checked).arg(linked).arg(repositories.size()).arg(checking ? tr(" · Checking…") : QString())
        + (scopeMachines.size()==1 ? " · "+*scopeMachines.cbegin() : QString())
        + tr(" · %1 cleanup candidates · %2%3 estimated").arg(candidates).arg(candidateComplete ? QString() : tr("at least "), QLocale().formattedDataSize(candidateBytes))
        + (partial ? tr(" · Catalog is incomplete; some folders are not shown.") : QString()));
    if(oldest>0) m_status->setText(m_status->text()+tr(" · Updated %1").arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(oldest).time(),QLocale::ShortFormat)));
    m_status->setToolTip(tr("Browsing data is cached for one hour. Refresh after external Git changes or when cleanup checks expire (five minutes). Cleanup rechecks each folder before removal."));
    showDetails(); reviewSelection();
}

void WorktreesDialog::showDetails()
{
    const auto *source = selectedSource(); const auto row = selectedRow();
    if (!source) { m_details->setContext({}, {}, {}, {}, {}, {}, false); updateActions(); return; }
    auto detail = reviewedRow(*source, row); if (detail.isEmpty()) detail = row;
    QJsonArray related;
    if (const auto *machine = box(*source)) for (const auto &session : machine->sessions) {
        const auto path = row["kind"] == "folder" ? session.canonicalCwd : session.gitRoot;
        if (path == row["path"].toString()) related.append(QJsonObject{{"name", session.name}, {"label", session.name + " · " + session.state}});
    }
    for (const auto &value : detail["sessions"].toArray()) {
        bool exists = false; for (const auto &item : related) exists |= item.toObject()["name"] == value;
        if (!exists) related.append(QJsonObject{{"name", value}, {"label", value}});
    }
    QString context = state(*source, row);
    if (!online(*source)) context += tr(" · Last known data; reconnect to check this folder.");
    if (!source->error.isEmpty()) context += "\n" + source->error;
    const auto reviewError = m_reviewErrors.value(key(source->host, source->catalog["common_dir"].toString()));
    if (!reviewError.isEmpty()) context += "\n" + reviewError;
    detail["context"] = context;
    m_details->setContext(source->host, source->machine, row["path"].toString(), source->catalog["common_dir"].toString(), detail, related,
        online(*source) && source->catalog["state"] == "ok" && row["available"].toBool());
    updateActions();
}

QList<QJsonObject> WorktreesDialog::selectedEntries() const
{
    QList<QJsonObject> entries;
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        const auto *item = *it; if (!item->parent() || !(item->flags() & Qt::ItemIsUserCheckable) || item->checkState(0) != Qt::Checked) continue;
        const auto source = m_sources.constFind(item->data(0, Qt::UserRole).toString()); if (source == m_sources.cend() || !canClean(*source)) continue;
        const auto row = item->data(0, Qt::UserRole + 1).toJsonObject(), review = reviewedRow(*source, row);
        if (!freshProof(*source,review) || !WorktreeReviewUi::removable(review) || review["fingerprint"].toString() != m_checked.value(key(source->host, row["path"].toString()))) continue;
        entries << QJsonObject{{"host", source->host}, {"machine", source->machine}, {"common_dir", source->catalog["common_dir"]}, {"row", review}};
    }
    return entries;
}

void WorktreesDialog::updateActions()
{
    const auto entries = selectedEntries(); m_remove->setEnabled(!entries.isEmpty()); m_remove->setText(tr("Review cleanup (%1)…").arg(entries.size()));
    m_copy->setEnabled(!selectedRow()["path"].toString().isEmpty()); m_refresh->setEnabled(m_requests.isEmpty() && !m_reviewRequest);
    bool candidate = false; for (QTreeWidgetItemIterator it(m_tree); *it; ++it) if ((*it)->parent() && ((*it)->flags() & Qt::ItemIsUserCheckable) && WorktreeReviewUi::category((*it)->data(0, Qt::UserRole + 4).toJsonObject()) == 0) candidate = true;
    m_cleanup->setEnabled(candidate);
}

void WorktreesDialog::reviewSelection()
{
    if (m_reviewRequest) return;
    for (const auto &source : m_sources) {
        if (!canClean(source)) continue;
        const auto common = source.catalog["common_dir"].toString(), repository = key(source.host, common);
        QString focus, attempt = repository;
        if (m_reviewAttempted.contains(repository)) {
            if (!m_reviews.contains(repository)) continue;
            for (const auto &value : source.catalog["worktrees"].toArray()) {
                const auto row = value.toObject(); if (row["kind"] != "linked") continue;
                const auto reviewed = reviewedRow(source, row); const auto path = row["path"].toString();
                const auto candidate = key(repository, path);
                if ((reviewed.isEmpty() || WorktreeReviewUi::hasReason(reviewed, {"status_unavailable"})) && !m_reviewAttempted.contains(candidate)) { focus = path; attempt = candidate; break; }
            }
            if (focus.isEmpty()) continue;
        }
        m_reviewAttempted.insert(attempt); m_reviewKey = repository; m_reviewWorktree = focus; m_reviewGeneration = m_generation;
        m_reviewRequest = m_client->reviewWorktrees(source.host, common, focus); updateActions(); return;
    }
}

void WorktreesDialog::cleanUp()
{
    const auto entries = selectedEntries(); if (entries.isEmpty()) return;
    WorktreeBatchCleanupDialog dialog(m_client, entries, this); dialog.exec();
    if (dialog.attempted()) {
        m_changed |= dialog.changed(); m_checked.clear();
        for (const auto &entry : entries) {
            const auto host = entry["host"].toString(), common = entry["common_dir"].toString(), repository = key(host, common);
            invalidateReview(repository);
            for (auto &source : m_sources) if (source.host == host && source.catalog["common_dir"] == common) { source.attempted = false; source.fresh = false; }
        }
        pump(); render();
    }
}

void WorktreesDialog::invalidateReview(const QString &repository)
{
    m_reviews.remove(repository); m_reviewErrors.remove(repository); m_reviewAttempted.remove(repository);
    for (auto it = m_reviewAttempted.begin(); it != m_reviewAttempted.end();) {
        const auto parts = QJsonDocument::fromJson(it->toUtf8()).array();
        if (!parts.isEmpty() && parts.first().toString() == repository) it = m_reviewAttempted.erase(it); else ++it;
    }
}
