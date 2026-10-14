#include "DirectoryDialog.h"
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
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

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
    m_newFolder = new QPushButton(tr("New folder…")); m_newFolder->setObjectName("newFolder");
    folderActions->addWidget(m_newFolder); layout->addLayout(folderActions);
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
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { navigate(item->data(Qt::UserRole).toString()); });
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (m_current.isEmpty()) return;
        m_choose->setText(item ? tr("Use selected folder") : tr("Use this folder"));
    });
    connect(m_path, &QLineEdit::textEdited, this, [this]() {
        m_request = 0; m_newFolder->setEnabled(true);
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
            auto *item = m_list->item(i); item->setHidden(!item->text().contains(query, Qt::CaseInsensitive));
            if (item->isHidden() && item == m_list->currentItem()) m_list->setCurrentRow(-1);
        }
    });
    connect(&m_client, &HgsClient::directoriesReady, this, [this](quint64 request, const QString &host, const QJsonObject &data) {
        if (request != m_request || host != m_host || m_creating) return;
        m_current = data.value("path").toString(); m_parent = data.value("parent").toString();
        m_path->setText(m_current); m_list->clear(); m_filter->clear();
        for (const auto &value : data.value("directories").toArray()) {
            const auto folder = value.toObject(); auto *item = new QListWidgetItem(folder.value("name").toString(), m_list);
            item->setData(Qt::UserRole, folder.value("path").toString());
            item->setToolTip(folder.value("path").toString());
        }
        m_list->setEnabled(true); m_choose->setEnabled(true); m_newFolder->setEnabled(true); m_choose->setText(tr("Use this folder"));
        m_status->setText(m_current == m_createdPath ? tr("Folder created. Press Use this folder to select it.") :
            data.value("truncated").toBool() ? tr("Showing the first 2,000 folders. You can also enter an exact path above.") :
            m_list->count() ? tr("Double-click to open a folder, or select it and press Use selected folder.") : tr("No subfolders. You can use this folder."));
    });
    connect(&m_client, &HgsClient::directoriesFailed, this, [this](quint64 request, const QString &host, const QString &error) {
        if (request != m_request || host != m_host || m_creating) return;
        m_status->setText(error); m_current.clear(); m_list->clear(); m_choose->setEnabled(false);
        m_newFolder->setEnabled(true);
    });
    connect(&m_client, &HgsClient::directoryCreated, this, [this](quint64 request, const QString &host, const QJsonObject &data) {
        if (request != m_createRequest || host != m_host || !m_creating) return;
        setCreating(false); m_createdPath = data.value("path").toString(); navigate(m_createdPath);
    });
    connect(&m_client, &HgsClient::directoryCreationFailed, this, [this](quint64 request, const QString &host, const QString &error) {
        if (request != m_createRequest || host != m_host || !m_creating) return;
        setCreating(false); m_current.clear(); m_list->clear(); m_choose->setEnabled(false); m_status->setText(error);
    });
    for (auto *button : findChildren<QPushButton *>()) button->setAutoDefault(false);
    navigate(path.isEmpty() ? "~" : path);
}

bool DirectoryDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            if (const auto *item = m_list->currentItem()) navigate(item->data(Qt::UserRole).toString());
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void DirectoryDialog::navigate(const QString &path)
{
    if (m_creating) return;
    m_path->setText(path); m_status->setText(tr("Loading folders…"));
    m_choose->setEnabled(false); m_list->setEnabled(false); m_newFolder->setEnabled(false);
    m_request = m_client.requestDirectories(m_host, path, m_hidden->isChecked());
}

void DirectoryDialog::reject()
{
    if (!m_creating) QDialog::reject();
}

void DirectoryDialog::setCreating(bool creating)
{
    m_creating = creating;
    for (auto *widget : QList<QWidget *>{m_up, m_home, m_go, m_path, m_filter, m_list, m_hidden, m_newFolder, m_cancel})
        widget->setEnabled(!creating);
    m_choose->setEnabled(!creating && !m_current.isEmpty());
}

void DirectoryDialog::newFolder()
{
    if (m_creating) return;
    QDialog prompt(this); prompt.setObjectName("createFolderDialog");
    prompt.setWindowTitle(tr("New folder — %1").arg(m_host.isEmpty() ? tr("This machine") : m_host));
    prompt.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&prompt);
    auto *description = new QLabel(tr("Create a folder on %1. Its parent folder must already exist.")
        .arg(m_host.isEmpty() ? tr("this computer") : m_host));
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); layout->addWidget(description);
    auto *path = new QLineEdit; path->setObjectName("newFolderPath"); path->setAccessibleName(tr("New folder path"));
    const auto typed = m_path->text();
    path->setText(!m_current.isEmpty() && typed == m_current ? m_current + (m_current.endsWith('/') ? "" : "/") : typed);
    auto *form = new QFormLayout; form->addRow(tr("Folder path"), path); layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *create = buttons->addButton(tr("Create folder"), QDialogButtonBox::AcceptRole);
    create->setObjectName("confirmCreateFolder"); create->setDefault(false); create->setAutoDefault(false);
    layout->addWidget(buttons);
    const auto update = [path, create] {
        const auto text = path->text();
        const bool absolute = text.startsWith('/') || (text.startsWith("~/") && !text.startsWith("~//"));
        const bool controls = std::any_of(text.cbegin(), text.cend(), [](QChar c) {
            return c.category() == QChar::Other_Control;
        });
        const auto leaf = text.section('/', -1);
        create->setEnabled(absolute && !controls && text.toUtf8().size() <= 4096
            && !leaf.isEmpty() && leaf != "." && leaf != ".." && text != "~");
    };
    connect(path, &QLineEdit::textChanged, &prompt, update); update();
    connect(create, &QPushButton::clicked, &prompt, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &prompt, &QDialog::reject);
    path->setFocus(); path->setCursorPosition(path->text().size());
    if (prompt.exec() != QDialog::Accepted) return;
    m_request = 0; m_createdPath.clear(); m_path->setText(path->text());
    m_status->setText(tr("Creating folder on %1…").arg(m_host.isEmpty() ? tr("this computer") : m_host));
    setCreating(true); m_createRequest = m_client.createDirectory(m_host, path->text());
}
