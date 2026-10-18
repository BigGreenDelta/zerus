#include "DirectoryDialog.h"
#include <QAction>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QScrollBar>
#include <QScreen>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QVBoxLayout>
#include <algorithm>

namespace {
bool validFolderName(const QString &name, const QString &parent)
{
    return !name.isEmpty() && name != "." && name != ".." && !name.contains('/')
        && (parent + '/' + name).toUtf8().size() <= 4096
        && std::none_of(name.cbegin(), name.cend(), [](QChar c) { return c.category() == QChar::Other_Control; });
}
QString childPath(const QString &parent, const QString &name)
{
    return parent + (parent.endsWith('/') ? "" : "/") + name;
}
}

QString DirectoryDialog::chooseDirectory(const QString &hgsPath, const QString &host,
                                         const QString &path, QWidget *parent)
{
    if (host.isEmpty()) {
        QString start = path;
        if (start.isEmpty() || start == "~") start = QDir::homePath();
        else if (start.startsWith("~/")) start = QDir::homePath() + start.mid(1);
        return QFileDialog::getExistingDirectory(parent, tr("Choose a folder"), start);
    }
    DirectoryDialog browser(hgsPath, host, path, parent);
    return browser.exec() == QDialog::Accepted ? browser.directory() : QString();
}

DirectoryDialog::DirectoryDialog(const QString &hgsPath, const QString &host,
                                 const QString &path, QWidget *parent)
    : QDialog(parent), m_client(hgsPath, this), m_host(host)
{
    setObjectName("folderBrowser"); setWindowTitle(tr("Choose folder — %1").arg(host.isEmpty() ? tr("This machine") : host));
    resize(660, 540); setMinimumSize(500, 400);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(24, 24, 24, 24); layout->setSpacing(14);
    auto *heading = new QLabel(tr("Choose a folder")); heading->setObjectName("detailTitle"); layout->addWidget(heading);
    auto *machine = new QLabel(tr("Folders on %1").arg(host.isEmpty() ? tr("this machine") : host));
    machine->setTextFormat(Qt::PlainText); machine->setObjectName("muted"); layout->addWidget(machine);
    auto *navigation = new QHBoxLayout;
    m_up = new QPushButton(tr("Up")); m_up->setObjectName("folderUp"); navigation->addWidget(m_up);
    m_home = new QPushButton(tr("Home")); navigation->addWidget(m_home);
    m_path = new QLineEdit; m_path->setObjectName("folderPath"); m_path->setAccessibleName(tr("Directory path")); navigation->addWidget(m_path, 1);
    m_go = new QPushButton(tr("Go")); m_go->setObjectName("folderGo"); navigation->addWidget(m_go); layout->addLayout(navigation);
    m_filter = new QLineEdit; m_filter->setPlaceholderText(tr("Filter folders in this directory…")); m_filter->setClearButtonEnabled(true); layout->addWidget(m_filter);
    m_list = new QListWidget; m_list->setObjectName("folderList"); m_list->setAccessibleName(tr("Directories")); layout->addWidget(m_list, 1);
    m_list->installEventFilter(this);
    auto *folderActions = new QHBoxLayout;
    m_hidden = new QCheckBox(tr("Show hidden folders")); folderActions->addWidget(m_hidden);
    folderActions->addStretch();
    m_newFolder = new QPushButton(tr("New folder")); m_newFolder->setObjectName("newFolder");
    folderActions->addWidget(m_newFolder);
    m_newFolderOptions = new QPushButton(tr("Dialog…")); m_newFolderOptions->setObjectName("newFolderOptions");
    m_newFolderOptions->setAccessibleName(tr("New folder dialog…"));
    m_newFolderOptions->setToolTip(tr("New folder dialog…"));
    auto *dialogAction = new QAction(tr("New folder dialog…"), this);
    dialogAction->setObjectName("newFolderDialog");
    connect(dialogAction, &QAction::triggered, this, &DirectoryDialog::newFolderDialog);
    connect(m_newFolderOptions, &QPushButton::clicked, dialogAction, &QAction::trigger);
    folderActions->addWidget(m_newFolderOptions); layout->addLayout(folderActions);
    m_status = new QLabel; m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText); m_status->setObjectName("muted"); layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_cancel = buttons->button(QDialogButtonBox::Cancel);
    m_choose = buttons->addButton(tr("Use this folder"), QDialogButtonBox::AcceptRole); m_choose->setObjectName("chooseFolder");
    m_choose->setDefault(false); m_choose->setAutoDefault(false); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_up, &QPushButton::clicked, this, [this]() { navigate(m_parent.isEmpty() ? "~" : m_parent); });
    connect(m_home, &QPushButton::clicked, this, [this]() { navigate("~"); });
    connect(m_go, &QPushButton::clicked, this, [this]() { navigate(m_path->text()); });
    connect(m_path, &QLineEdit::returnPressed, m_go, &QPushButton::click);
    connect(m_newFolder, &QPushButton::clicked, this, &DirectoryDialog::newFolder);
    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), this);
    connect(shortcut, &QShortcut::activated, m_newFolder, &QPushButton::click);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this, dialogAction](const QPoint &point) {
        QMenu menu(this);
        auto *create = menu.addAction(tr("New folder"), m_newFolder, &QPushButton::click);
        create->setEnabled(m_newFolder->isEnabled());
        menu.addAction(dialogAction); menu.exec(m_list->viewport()->mapToGlobal(point));
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item != m_inlineItem) navigate(item->data(Qt::UserRole).toString());
    });
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (m_current.isEmpty()) return;
        m_choose->setText(item && item != m_inlineItem ? tr("Use selected folder") : tr("Use this folder"));
    });
    connect(m_path, &QLineEdit::textEdited, this, [this]() {
        cancelInlineFolder(); m_request = 0; m_creationError.clear();
        m_newFolder->setEnabled(true); m_newFolderOptions->setEnabled(true); m_list->setEnabled(false);
        m_choose->setEnabled(false); m_status->setText(tr("Press Go to open this path, or New folder to create it."));
    });
    connect(m_choose, &QPushButton::clicked, this, [this]() {
        if (const auto *item = m_list->currentItem()) {
            m_current = item->data(Qt::UserRole).toString();
        }
        accept();
    });
    connect(m_hidden, &QCheckBox::toggled, this, [this]() { navigate(m_current.isEmpty() ? "~" : m_current); });
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &query) {
        for (int i = 0; i < m_list->count(); ++i) {
            auto *item = m_list->item(i); if (item == m_inlineItem) continue;
            item->setHidden(!item->text().contains(query, Qt::CaseInsensitive));
            if (item->isHidden() && item == m_list->currentItem()) m_list->setCurrentRow(-1);
        }
    });
    connect(&m_client, &HgsClient::directoriesReady, this, [this](quint64 request, const QString &host, const QJsonObject &data) {
        if (request != m_request || host != m_host || m_creating) return;
        m_request = 0; cancelInlineFolder();
        m_current = data.value("path").toString(); m_parent = data.value("parent").toString();
        m_path->setText(m_current); m_list->clear(); m_filter->clear();
        for (const auto &value : data.value("directories").toArray()) {
            const auto folder = value.toObject(); auto *item = new QListWidgetItem(folder.value("name").toString(), m_list);
            item->setData(Qt::UserRole, folder.value("path").toString());
            item->setToolTip(folder.value("path").toString());
        }
        m_list->setEnabled(true); m_choose->setEnabled(true); m_newFolder->setEnabled(true); m_newFolderOptions->setEnabled(true);
        m_choose->setText(tr("Use this folder"));
        m_status->setText(!m_creationError.isEmpty() ? tr("The folder is available. Press Use this folder to select it.") :
            m_current == m_createdPath ? tr("Folder created. Press Use this folder to select it.") :
            data.value("truncated").toBool() ? tr("Showing the first 2,000 folders. You can also enter an exact path above.") :
            m_list->count() ? tr("Double-click to open a folder, or select it and press Use selected folder.") : tr("No subfolders. You can use this folder."));
    });
    connect(&m_client, &HgsClient::directoriesFailed, this, [this](quint64 request, const QString &host, const QString &error) {
        if (request != m_request || host != m_host || m_creating) return;
        m_request = 0;
        m_status->setText(m_creationError.isEmpty() ? error : m_creationError);
        m_current.clear(); m_list->clear(); m_list->setEnabled(true); m_choose->setEnabled(false);
        m_newFolder->setEnabled(true); m_newFolderOptions->setEnabled(true);
    });
    connect(&m_client, &HgsClient::directoryCreated, this, [this](quint64 request, const QString &host, const QJsonObject &data) {
        if (request != m_createRequest || host != m_host || !m_creating) return;
        setCreating(false); m_createdPath = data.value("path").toString(); navigate(m_createdPath);
    });
    connect(&m_client, &HgsClient::directoryCreationFailed, this, [this](quint64 request, const QString &host, const QString &error) {
        if (request != m_createRequest || host != m_host || !m_creating) return;
        setCreating(false); m_current.clear(); m_list->clear(); m_choose->setEnabled(false);
        // mkdir may have succeeded before its receipt was lost, or the path may
        // already exist. A fresh read can prove it selectable without replaying mkdir.
        navigate(m_path->text()); m_creationError = error;
    });
    for (auto *button : findChildren<QPushButton *>()) button->setAutoDefault(false);
    navigate(path.isEmpty() ? "~" : path);
}

bool DirectoryDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (m_inlineItem && watched == m_list->itemWidget(m_inlineItem)
        && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange))
        sizeInlineFolder();
    if (watched == m_inlineName && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        cancelInlineFolder(); m_list->setFocus(); return true;
    }
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (const auto *item = m_list->currentItem(); item && item != m_inlineItem)
                navigate(item->data(Qt::UserRole).toString());
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void DirectoryDialog::navigate(const QString &path)
{
    if (m_creating) return;
    cancelInlineFolder(); m_creationError.clear();
    m_path->setText(path); m_status->setText(tr("Loading folders…"));
    m_choose->setEnabled(false); m_list->setEnabled(false); m_newFolder->setEnabled(false); m_newFolderOptions->setEnabled(false);
    m_request = m_client.requestDirectories(m_host, path, m_hidden->isChecked());
}

void DirectoryDialog::reject()
{
    if (!m_creating) QDialog::reject();
}

void DirectoryDialog::setCreating(bool creating)
{
    m_creating = creating;
    for (auto *widget : QList<QWidget *>{m_up, m_home, m_go, m_path, m_filter, m_list, m_hidden, m_newFolder, m_newFolderOptions, m_cancel})
        widget->setEnabled(!creating);
    m_choose->setEnabled(!creating && !m_current.isEmpty());
}

void DirectoryDialog::newFolder()
{
    if (m_creating || m_request) return;
    if (m_current.isEmpty() || m_path->text() != m_current) { newFolderDialog(); return; }
    if (m_inlineName) { m_inlineName->setFocus(); return; }
    const auto parent = m_current;
    m_inlineListMinimumHeight = m_list->minimumHeight();
    m_inlineDialogMinimumSize = minimumSize();
    m_filter->clear(); m_list->clearSelection();
    m_inlineItem = new QListWidgetItem;
    m_list->insertItem(0, m_inlineItem);
    auto *row = new QWidget; auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    m_inlineName = new QLineEdit; m_inlineName->setObjectName("inlineFolderName");
    m_inlineName->setPlaceholderText(tr("Folder name")); m_inlineName->setAccessibleName(tr("New folder name"));
    m_inlineName->installEventFilter(this); layout->addWidget(m_inlineName, 1);
    auto *create = new QPushButton(tr("Create")); create->setObjectName("createInlineFolder");
    create->setAutoDefault(false); create->setEnabled(false); layout->addWidget(create);
    auto *cancel = new QPushButton(tr("Cancel")); cancel->setAutoDefault(false);
    cancel->setObjectName("cancelInlineFolder"); cancel->setAccessibleName(tr("Cancel new folder"));
    cancel->setToolTip(tr("Cancel new folder")); layout->addWidget(cancel);
    m_list->setItemWidget(m_inlineItem, row);
    row->installEventFilter(this); sizeInlineFolder();
    m_list->setCurrentItem(m_inlineItem); m_choose->setEnabled(false);
    connect(m_inlineName, &QLineEdit::textChanged, row, [this, parent, create](const QString &name) {
        create->setEnabled(validFolderName(name, parent));
    });
    connect(m_inlineName, &QLineEdit::returnPressed, create, &QPushButton::click);
    connect(create, &QPushButton::clicked, this, [this, parent] {
        if (!m_inlineName || !validFolderName(m_inlineName->text(), parent)) return;
        createFolder(childPath(parent, m_inlineName->text()));
    });
    connect(cancel, &QPushButton::clicked, this, &DirectoryDialog::cancelInlineFolder);
    m_list->scrollToItem(m_inlineItem); m_inlineName->setFocus();
}

void DirectoryDialog::sizeInlineFolder()
{
    if (!m_inlineItem) return;
    auto *row = m_list->itemWidget(m_inlineItem);
    if (!row) return;
    row->ensurePolished();
    for (auto *child : row->findChildren<QWidget *>()) child->ensurePolished();
    row->layout()->activate();
    // The index widget occupies the styled item's content box. Reserve its
    // padding too, or the list clips the editor below its styled size hint.
    QStyleOptionViewItem option; option.initFrom(m_list);
    option.index = m_list->indexFromItem(m_inlineItem);
    option.rect = QRect(QPoint(), row->sizeHint());
    const auto contents = m_list->style()->subElementRect(QStyle::SE_ItemViewItemText, &option, m_list);
    // Do not force the editor's preferred width onto the entire list: the
    // layout can shrink the name field while keeping its buttons available.
    const QSize size(0, row->sizeHint().height() + qMax(0, option.rect.height() - contents.height()));
    if (m_inlineItem->sizeHint() != size) m_inlineItem->setSizeHint(size);
    m_list->setMinimumHeight(qMax(m_inlineListMinimumHeight,
        size.height() + 2 * m_list->frameWidth() + m_list->horizontalScrollBar()->sizeHint().height()));
    // Large system text can need more room than the normal 500x400 floor.
    // Respect the whole layout too, so later controls cannot cover the row.
    layout()->invalidate(); layout()->activate();
    auto minimum = minimumSizeHint();
    minimum.setHeight(qMax(minimum.height(), layout()->totalHeightForWidth(minimum.width())));
    if (screen()) minimum = minimum.boundedTo(screen()->availableGeometry().size());
    setMinimumSize(m_inlineDialogMinimumSize.expandedTo(minimum));
    layout()->activate();
}

void DirectoryDialog::cancelInlineFolder()
{
    if (!m_inlineItem) return;
    auto *item = m_inlineItem; m_inlineItem = nullptr; m_inlineName = nullptr;
    m_list->setMinimumHeight(m_inlineListMinimumHeight);
    setMinimumSize(m_inlineDialogMinimumSize);
    delete m_list->takeItem(m_list->row(item));
    m_list->setCurrentRow(-1); m_list->clearSelection();
    m_choose->setText(tr("Use this folder"));
    m_choose->setEnabled(!m_creating && !m_request && !m_current.isEmpty() && m_path->text() == m_current);
}

void DirectoryDialog::newFolderDialog()
{
    if (m_creating || m_request) return;
    const auto typed = m_path->text();
    const bool current = !m_current.isEmpty() && typed == m_current;
    const auto split = typed.lastIndexOf('/');
    const auto parent = current ? m_current : split == 0 ? QStringLiteral("/") : typed.left(split);
    if (parent != "~" && !parent.startsWith('/') && !parent.startsWith("~/")) return;
    const auto initialName = m_inlineName ? m_inlineName->text() : current ? QString() : typed.mid(split + 1);
    cancelInlineFolder();
    QDialog prompt(this); prompt.setObjectName("createFolderDialog");
    prompt.setWindowTitle(tr("New folder — %1").arg(m_host.isEmpty() ? tr("This machine") : m_host));
    prompt.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&prompt);
    auto *description = new QLabel(tr("Create a folder in %1 on %2.")
        .arg(parent, m_host.isEmpty() ? tr("this computer") : m_host));
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); layout->addWidget(description);
    auto *name = new QLineEdit(initialName); name->setObjectName("newFolderName"); name->setAccessibleName(tr("New folder name"));
    auto *form = new QFormLayout; form->addRow(tr("Folder name"), name); layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *create = buttons->addButton(tr("Create folder"), QDialogButtonBox::AcceptRole);
    create->setObjectName("confirmCreateFolder"); create->setDefault(false); create->setAutoDefault(false);
    layout->addWidget(buttons);
    const auto update = [name, create, parent] { create->setEnabled(validFolderName(name->text(), parent)); };
    connect(name, &QLineEdit::textChanged, &prompt, update); update();
    connect(name, &QLineEdit::returnPressed, create, &QPushButton::click);
    connect(create, &QPushButton::clicked, &prompt, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &prompt, &QDialog::reject);
    for (auto *button : prompt.findChildren<QPushButton *>()) button->setAutoDefault(false);
    name->setFocus(); name->selectAll();
    if (prompt.exec() != QDialog::Accepted) return;
    createFolder(childPath(parent, name->text()));
}

void DirectoryDialog::createFolder(const QString &path)
{
    if (m_creating) return;
    cancelInlineFolder(); m_request = 0; m_createdPath.clear(); m_creationError.clear(); m_path->setText(path);
    m_status->setText(tr("Creating folder on %1…").arg(m_host.isEmpty() ? tr("this computer") : m_host));
    setCreating(true); m_createRequest = m_client.createDirectory(m_host, path);
}
