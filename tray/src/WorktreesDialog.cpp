#include "WorktreesDialog.h"
#include "DialogButtons.h"
#include "WorktreeCleanupDialog.h"
#include <QApplication>
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
    setMinimumSize(760, 500); resize(1000, 640);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(24, 22, 24, 22); layout->setSpacing(12);
    auto *heading = new QLabel(tr("Worktrees")); heading->setObjectName("heading");
    auto *headingRow = new QHBoxLayout; headingRow->addWidget(heading); headingRow->addStretch();
    m_refresh = new QPushButton(tr("Refresh")); m_refresh->setObjectName("refreshWorktreeOverview");
    headingRow->addWidget(m_refresh); layout->addLayout(headingRow);
    auto *hint = new QLabel(tr("Working folders across your projects and computers. Select a repository to check what can be cleaned up."));
    hint->setObjectName("muted"); hint->setWordWrap(true); layout->addWidget(hint);
    auto *filters = new QHBoxLayout;
    m_project = new QComboBox; m_project->setObjectName("worktreeProjectFilter"); m_project->setAccessibleName(tr("Project"));
    m_computer = new QComboBox; m_computer->setObjectName("worktreeComputerFilter"); m_computer->setAccessibleName(tr("Computer"));
    for (auto *filter : {m_project, m_computer}) {
        filter->setMinimumContentsLength(12); filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    }
    filters->addWidget(new QLabel(tr("Project"))); filters->addWidget(m_project, 1);
    filters->addWidget(new QLabel(tr("Computer"))); filters->addWidget(m_computer, 1); layout->addLayout(filters);
    m_tree = new QTreeWidget; m_tree->setObjectName("worktreeOverviewList"); m_tree->setColumnCount(5);
    m_tree->setHeaderLabels({tr("Worktree / folder"), tr("Branch"), tr("Computer"), tr("Sessions"), tr("State")});
    m_tree->setRootIsDecorated(false); m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column) m_tree->header()->setSectionResizeMode(column, QHeaderView::Interactive);
    m_tree->setColumnWidth(1, 140); m_tree->setColumnWidth(2, 100); m_tree->setColumnWidth(3, 70); m_tree->setColumnWidth(4, 125);
    layout->addWidget(m_tree, 1);
    m_details = new QPlainTextEdit; m_details->setObjectName("worktreeOverviewDetails"); m_details->setReadOnly(true);
    m_details->setAccessibleName(tr("Worktree details")); m_details->setMaximumHeight(150); layout->addWidget(m_details);
    m_status = new QLabel; m_status->setObjectName("worktreeOverviewStatus"); m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true); layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox;
    m_copy = buttons->addButton(tr("Copy path"), QDialogButtonBox::ActionRole); m_copy->setObjectName("copyOverviewWorktreePath");
    m_cleanup = buttons->addButton(tr("Clean up repository…"), QDialogButtonBox::ActionRole); m_cleanup->setObjectName("cleanUpOverviewRepository");
    m_remove = buttons->addButton(tr("Remove worktree…"), QDialogButtonBox::ActionRole); m_remove->setObjectName("removeOverviewWorktree");
    auto *close = buttons->addButton(QDialogButtonBox::Close);
    for (auto *button : {m_refresh, m_copy, m_cleanup, m_remove, close}) button->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_refresh, &QPushButton::clicked, this, &WorktreesDialog::refresh);
    connect(m_project, &QComboBox::currentIndexChanged, this, [this] { render(); });
    connect(m_computer, &QComboBox::currentIndexChanged, this, [this] { render(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] { showDetails(); reviewSelection(); });
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(selectedRow()["path"].toString()); });
    connect(m_cleanup, &QPushButton::clicked, this, [this] { cleanUp(false); });
    connect(m_remove, &QPushButton::clicked, this, [this] { cleanUp(true); });
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
                if (data["state"] == "ok" && key(host, data["common_dir"].toString()) == m_reviewKey) m_reviews[m_reviewKey] = data;
                else m_reviewErrors[m_reviewKey] = data["detail"].toString(tr("Cleanup review is unavailable."));
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
        m_context = context; ++m_generation;
        m_sources = sources; m_reviews.clear(); m_reviewErrors.clear(); m_reviewAttempted.clear();
        for (auto &source : m_sources) source.catalog = m_client->worktreeSnapshot(source.host, source.path);
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
            m_reviews.remove(repository); m_reviewAttempted.remove(repository); m_reviewErrors.remove(repository);
        }
    }
    m_fleet = fleet; render(); pump();
}

void WorktreesDialog::refresh()
{
    if (!m_requests.isEmpty() || m_reviewRequest) return;
    ++m_generation; m_reviews.clear(); m_reviewErrors.clear(); m_reviewAttempted.clear();
    for (auto &source : m_sources) { source.attempted = false; source.fresh = false; source.error.clear(); }
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
    if (!source.fresh || !source.error.isEmpty() || !online(source)) return {};
    const auto review = m_reviews.value(key(source.host, source.catalog["common_dir"].toString()));
    for (const auto &value : review["worktrees"].toArray()) if (value.toObject()["path"] == row["path"]) return value.toObject();
    return {};
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
    if (verdict == "ready") return tr("Ready");
    if (verdict == "review") return tr("Review");
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
    const auto selected = m_tree->currentItem() ? m_tree->currentItem()->data(0, Qt::UserRole + 2).toString() : QString();
    const auto scroll = m_tree->verticalScrollBar()->value();
    // Combine links from several projects and subdirectories without combining
    // identical paths on different computers.
    QMap<QString, QStringList> repositories;
    for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) {
        const auto &source = it.value();
        if (!m_project->currentData().toString().isEmpty() && !source.projectIds.contains(m_project->currentData().toString())) continue;
        if (!m_computer->currentData().toString().isEmpty() && source.machine != m_computer->currentData().toString()) continue;
        const auto common = source.catalog["common_dir"].toString();
        repositories[key(source.host, common.isEmpty() ? source.path : common)].append(it.key());
    }
    int linked = 0; QJsonArray rendered;
    for (const auto &links : repositories) {
        auto sourceKey = links.first();
        for (const auto &link : links) if (m_sources[link].fresh && m_sources[link].error.isEmpty()) { sourceKey = link; break; }
        const auto &source = m_sources[sourceKey];
        QStringList projects;
        for (const auto &link : links) projects.append(m_sources[link].projectNames);
        projects.removeDuplicates(); projects.sort(Qt::CaseInsensitive);
        auto rows = source.catalog["worktrees"].toArray();
        if (rows.isEmpty()) rows.append(QJsonObject{{"path", source.path}, {"kind", "folder"}});
        for (const auto &value : rows) {
            const auto row = value.toObject(); const auto path = row["path"].toString();
            auto branch = row["branch"].toString();
            if (row["detached"].toBool()) branch = tr("Detached %1").arg(row["head"].toString().left(8));
            linked += row["kind"] == "linked";
            rendered.append(QJsonObject{{"source", sourceKey}, {"row", row}, {"identity", key(source.host, path)},
                {"projects", QJsonArray::fromStringList(projects)}, {"columns", QJsonArray{
                    QFileInfo(path).fileName() + '\n' + path, branch, source.machine,
                    QString::number(sessions(source, row).size()), state(source, row)}}});
        }
    }
    const auto fingerprint = QJsonDocument(rendered).toJson(QJsonDocument::Compact);
    if (fingerprint != m_rendered) {
        m_rendered = fingerprint; const QSignalBlocker block(m_tree); m_tree->clear();
        for (const auto &value : rendered) {
            const auto entry = value.toObject(); const auto row = entry["row"].toObject();
            QStringList columns, projects;
            for (const auto &column : entry["columns"].toArray()) columns.append(column.toString());
            for (const auto &project : entry["projects"].toArray()) projects.append(project.toString());
            auto *item = new QTreeWidgetItem(m_tree, columns);
            item->setData(0, Qt::UserRole, entry["source"].toString()); item->setData(0, Qt::UserRole + 1, row);
            item->setData(0, Qt::UserRole + 2, entry["identity"].toString()); item->setData(0, Qt::UserRole + 3, projects);
            for (int column = 0; column < 5; ++column) item->setToolTip(column, row["path"].toString() + '\n' + tr("Projects: %1").arg(projects.join(", ")));
            if (entry["identity"].toString() == selected) m_tree->setCurrentItem(item);
        }
        if (!m_tree->currentItem() && m_tree->topLevelItemCount()) m_tree->setCurrentItem(m_tree->topLevelItem(0));
        m_tree->verticalScrollBar()->setValue(scroll);
    }
    m_status->setText(m_sources.isEmpty() ? tr("Add a project folder to see its worktrees.")
        : m_tree->topLevelItemCount() == 0 ? tr("No folders match these filters.")
        : tr("%1 linked worktrees · %2 repositories / folders").arg(linked).arg(repositories.size()));
    showDetails(); reviewSelection();
}

void WorktreesDialog::showDetails()
{
    const auto *source = selectedSource(); const auto row = selectedRow();
    QStringList lines;
    if (source) {
        lines << tr("Computer: %1").arg(source->machine) << tr("Path: %1").arg(row["path"].toString());
        lines << tr("Projects: %1").arg(m_tree->currentItem()->data(0, Qt::UserRole + 3).toStringList().join(", "));
        if (!row["branch"].toString().isEmpty()) lines << tr("Branch: %1").arg(row["branch"].toString());
        if (!row["head"].toString().isEmpty()) lines << tr("Commit: %1").arg(row["head"].toString());
        const auto common = source->catalog["common_dir"].toString();
        if (!common.isEmpty()) lines << tr("Git directory: %1").arg(common);
        lines << tr("State: %1").arg(state(*source, row));
        if (!online(*source)) lines << tr("Last known information. Reconnect this computer to review or remove worktrees.");
        if (!source->error.isEmpty()) lines << source->error;
        if (source->catalog["partial"].toBool()) lines << tr("Partial catalog: only the first 512 worktrees are shown.");
        if (!source->catalog["detail"].toString().isEmpty()) lines << source->catalog["detail"].toString();
        for (const auto &field : {"locked_reason", "prunable_reason"}) if (!row[field].toString().isEmpty()) lines << row[field].toString();
        const auto review = reviewedRow(*source, row);
        const auto repositoryReview = m_reviews.value(key(source->host, common));
        if (repositoryReview["partial"].toBool()) lines << tr("Partial cleanup review. Other worktrees still need to be checked.");
        if (repositoryReview["sampled_at"].toDouble() > 0) lines << tr("Cleanup checked: %1").arg(QDateTime::fromSecsSinceEpoch(qint64(repositoryReview["sampled_at"].toDouble())).toLocalTime().toString("d MMM HH:mm:ss"));
        for (const auto &field : {"reasons", "notes"}) for (const auto &value : review[field].toArray()) lines << value.toObject()["message"].toString();
        if (review["ignored_bytes"].toDouble() > 0) lines << tr("Ignored files removed with the folder: %1").arg(QLocale().formattedDataSize(qint64(review["ignored_bytes"].toDouble())));
        const auto reviewKey = key(source->host, common);
        if (m_reviewErrors.contains(reviewKey)) lines << m_reviewErrors.value(reviewKey);
        if (m_reviewRequest && m_reviewKey == reviewKey) lines << tr("Checking cleanup on this computer…");
        const auto related = sessions(*source, row);
        if (!related.isEmpty()) lines << tr("%1 sessions:\n%2").arg(online(*source) ? tr("Related") : tr("Last recorded"), related.join('\n'));
    }
    const auto details = lines.isEmpty() ? tr("Select a worktree to see its details.") : lines.join('\n');
    if (m_details->toPlainText() != details) m_details->setPlainText(details);
    const bool available = source && canClean(*source);
    m_copy->setEnabled(source && !row["path"].toString().isEmpty());
    m_cleanup->setEnabled(available); m_remove->setEnabled(available && row["kind"] == "linked");
    m_remove->setText(row["available"].toBool() ? tr("Remove worktree…") : tr("Forget missing worktree…"));
    m_cleanup->setToolTip(tr("Review all linked worktrees of the selected repository before removal."));
    m_remove->setToolTip(tr("Review this worktree and its safety checks before removal."));
    m_refresh->setEnabled(m_requests.isEmpty() && !m_reviewRequest);
}

void WorktreesDialog::reviewSelection()
{
    const auto *source = selectedSource();
    if (!source || !canClean(*source) || m_reviewRequest) return;
    const auto common = source->catalog["common_dir"].toString(), reviewKey = key(source->host, common);
    if (m_reviewAttempted.contains(reviewKey)) return;
    m_reviewAttempted.insert(reviewKey); m_reviewKey = reviewKey; m_reviewGeneration = m_generation;
    m_reviewRequest = m_client->reviewWorktrees(source->host, common); showDetails();
}

void WorktreesDialog::cleanUp(bool focused)
{
    const auto *source = selectedSource(); const auto row = selectedRow();
    if (!source || !canClean(*source) || (focused && row["kind"] != "linked")) return;
    const auto host = source->host, machine = source->machine, common = source->catalog["common_dir"].toString();
    WorktreeCleanupDialog dialog(m_client, host, machine, common, focused ? row["path"].toString() : QString(), this);
    dialog.exec();
    if (dialog.changed()) {
        m_changed = true;
        // Invalidate every link of this repository, even if the catalog/project
        // changed while the modal cleanup was open.
        const auto repository = key(host, common); m_reviews.remove(repository); m_reviewErrors.remove(repository); m_reviewAttempted.remove(repository);
        for (auto &entry : m_sources) if (entry.host == host && entry.catalog["common_dir"].toString() == common) {
            entry.attempted = false; entry.fresh = false;
        }
        pump(); render();
    }
}
