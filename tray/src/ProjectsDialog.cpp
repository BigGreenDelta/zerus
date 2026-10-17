#include "ProjectsDialog.h"
#include "WorktreePanel.h"
#include "WorktreesDialog.h"
#include "ProjectAppearance.h"
#include "IdentityBadge.h"
#include <QStyledItemDelegate>
#include <QGridLayout>
#include <QCheckBox>
#include "DirectoryDialog.h"
#include "WorkspaceIcons.h"
#include "WorkspaceList.h"
#include "WorkspaceColorDialog.h"
#include <QCollator>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QAction>
#include <QMenu>
#include <QPainter>
#include <QTimer>
#include <QUrl>

namespace {
class FolderSplitterHandle : public QSplitterHandle {
public:
    using QSplitterHandle::QSplitterHandle;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);painter.setPen(palette().color(QPalette::Mid));
        const int y=rect().center().y();painter.drawLine(0,y,width()-1,y);
    }
};
class FolderSplitter : public QSplitter {
public:
    using QSplitter::QSplitter;
protected:
    QSplitterHandle *createHandle() override {return new FolderSplitterHandle(orientation(),this);}
};
class FolderSortItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem &other) const override {
        QCollator collator;collator.setCaseSensitivity(Qt::CaseInsensitive);collator.setNumericMode(true);
        const int order=collator.compare(text(),other.text());
        return order ? order<0 : data(Qt::UserRole).toString()<other.data(Qt::UserRole).toString();
    }
};
class FolderMachineDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        QStyleOptionViewItem base(option);initStyleOption(&base,index);base.text.clear();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem,&base,p,option.widget);
        const auto machine=index.data().toString();const bool dark=option.widget->property("hgsDark").toBool();
        const int width=IdentityBadges::width(IdentityBadges::Machine,machine,option.font,option.rect.width()-20);
        IdentityBadges::paint(p,QRect(option.rect.left()+10,option.rect.center().y()-9,width,18),IdentityBadges::Machine,machine,dark);
    }
};
class ProjectDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &,const QModelIndex &) const override {return {210,54};}
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        const auto rect=option.rect.adjusted(5,6,-5,-6);
        ProjectAppearance::paint(p,rect,index.data().toString(),index.data(Qt::UserRole+1).value<QColor>(),
            option.widget->property("hgsDark").toBool(),index.data(Qt::UserRole+2).toBool(),index.data(Qt::UserRole+3).toBool()?QObject::tr("Default"):QString());
        if(option.state & QStyle::State_Selected) {p->save();p->setRenderHint(QPainter::Antialiasing);p->setPen(QPen(option.widget->property("hgsDark").toBool()?QColor("#e2e9ef"):QColor("#344454"),1.5));p->setBrush(Qt::NoBrush);p->drawRoundedRect(rect.adjusted(-2,-2,2,2),8,8);p->restore();}
    }
};
}

ProjectsDialog::ProjectsDialog(HgsClient *client, QWidget *parent, bool embedded, SessionOrganization *projects)
    : QDialog(parent), m_client(client),
      m_ownedProjects(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object()),
      m_projects(projects ? projects : &m_ownedProjects)
{
    setObjectName("projectsPage"); setWindowTitle(tr("Projects")); resize(850, 560);
    if (embedded) setWindowFlags(Qt::Widget);
    auto *root = new QVBoxLayout(this); root->setContentsMargins(24, 22, 24, 18); root->setSpacing(14);
    auto *heading = new QLabel(tr("Projects")); heading->setObjectName("heading");
    auto *headingRow=new QHBoxLayout;headingRow->addWidget(heading);headingRow->addStretch();
    auto *worktrees = new QPushButton(tr("Worktrees…")); worktrees->setObjectName("projectWorktrees"); worktrees->setAutoDefault(false);
    worktrees->setToolTip(tr("View worktrees across projects and computers, and review cleanup.")); headingRow->addWidget(worktrees);
    connect(worktrees, &QPushButton::clicked, this, [this] {
        WorktreesDialog dialog(m_client, *m_projects, m_fleet, m_allProjects->isChecked(), this);
        dialog.sessionRequested = [this](const QString &host, const QString &name) { emit relatedSessionRequested(host, name, {}); };
        m_worktreeOverview = &dialog;
        dialog.exec();
        m_worktreeOverview.clear();
        if (dialog.changed()) m_worktrees->refreshCatalog();
    });
    auto *swarm=new QPushButton(tr("Swarm…"));swarm->setObjectName("projectSwarm");headingRow->addWidget(swarm);root->addLayout(headingRow);
    connect(swarm,&QPushButton::clicked,this,&ProjectsDialog::swarmRequested);
    auto *hint = new QLabel(tr("Organize sessions and folders across your computers. A folder can be used by several projects."));
    hint->setWordWrap(true); hint->setObjectName("muted"); root->addWidget(hint);
    auto *catalogRow=new QHBoxLayout;
    m_allProjects=new QCheckBox(tr("All swarm projects"));m_allProjects->setObjectName("allSwarmProjects");
    m_allProjects->setToolTip(tr("Include projects on computers without a configured connection here."));
    m_allProjects->setChecked(QSettings().value("workspace/allSwarmProjects",false).toBool());catalogRow->addWidget(m_allProjects);
    m_swarmStatus=new QLabel; m_swarmStatus->setObjectName("projectSwarmStatus");m_swarmStatus->setTextFormat(Qt::PlainText);m_swarmStatus->setWordWrap(true);catalogRow->addWidget(m_swarmStatus,1);root->addLayout(catalogRow);
    connect(m_allProjects,&QCheckBox::toggled,this,[this](bool checked){QSettings().setValue("workspace/allSwarmProjects",checked);refresh();});
    auto *split = new QSplitter; root->addWidget(split, 1);
    auto *left = new QWidget; auto *leftLayout = new QVBoxLayout(left); leftLayout->setContentsMargins(0,0,12,0);
    m_list = new WorkspaceList; m_list->setObjectName("logicalProjects"); m_list->setItemDelegate(new ProjectDelegate(m_list)); leftLayout->addWidget(m_list,1);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setDragDropOverwriteMode(false);m_list->setDropIndicatorShown(true);
    m_list->setAccessibleName(tr("Projects; drag to reorder, or use Alt+Up and Alt+Down"));
    m_moveUp=new QAction(tr("Move project up"),m_list);m_moveUp->setObjectName("moveProjectUp");
    m_moveDown=new QAction(tr("Move project down"),m_list);m_moveDown->setObjectName("moveProjectDown");
    m_moveUp->setShortcut(QKeySequence(Qt::ALT|Qt::Key_Up));m_moveDown->setShortcut(QKeySequence(Qt::ALT|Qt::Key_Down));
    for(auto *action:{m_moveUp,m_moveDown}){action->setShortcutContext(Qt::WidgetShortcut);m_list->addAction(action);}
    connect(m_moveUp,&QAction::triggered,this,[this]{moveProject(-1);});connect(m_moveDown,&QAction::triggered,this,[this]{moveProject(1);});
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list,&QListWidget::customContextMenuRequested,this,[this](const QPoint &position){
        auto *item=m_list->itemAt(position);if(item)m_list->setCurrentItem(item);
        auto *menu=new QMenu(this);menu->setObjectName("projectListMenu");
        auto *add=menu->addAction(tr("New project…"),this,&ProjectsDialog::addProject);add->setObjectName("addProjectAction");
        if(item){menu->addSeparator();menu->addAction(m_moveUp);menu->addAction(m_moveDown);}
        connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);menu->popup(m_list->viewport()->mapToGlobal(position));
    });
    connect(m_list->model(),&QAbstractItemModel::rowsMoved,this,[this]{
        QStringList order;for(int row=0;row<m_list->count();++row)order.append(m_list->item(row)->data(Qt::UserRole).toString());
        // Let Qt finish moving the row before rebuilding its items.
        QTimer::singleShot(0,this,[this,order]{
            QString before;for(auto it=order.crbegin();it!=order.crend();++it){m_projects->moveGroup(*it,before);before=*it;}
            changed();
        });
    });
    auto *add = new QPushButton(tr("New project…")); add->setObjectName("newProject"); leftLayout->addWidget(add); split->addWidget(left);
    auto *right = new QWidget; auto *form = new QVBoxLayout(right); form->setContentsMargins(12,0,0,0); form->setSpacing(12);
    auto *metadata = new QWidget; metadata->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Maximum);
    auto *fields = new QFormLayout(metadata); fields->setContentsMargins(0,0,0,0);
    m_name = new QLineEdit; m_name->setObjectName("projectName"); m_name->setMaxLength(80); m_name->setMaximumWidth(340);
    m_id = new QLabel; m_id->setObjectName("projectId"); m_id->setTextFormat(Qt::PlainText); m_id->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *idLabel = new QLabel(tr("ID")); idLabel->setObjectName("muted");
    auto *nameRow = new QHBoxLayout; nameRow->setSpacing(8);
    nameRow->addWidget(m_name,1); nameRow->addSpacing(8); nameRow->addWidget(idLabel); nameRow->addWidget(m_id); nameRow->addStretch();
    m_color = new QPushButton(tr("Edit color…")); m_color->setObjectName("projectColor");
    auto *colorRow=new QHBoxLayout; m_preview=new ProjectPreview; m_preview->setObjectName("projectColorPreview");
    colorRow->addWidget(m_preview,1);colorRow->addWidget(m_color);colorRow->addStretch();
    fields->addRow(tr("Name"),nameRow); fields->addRow(tr("Color"),colorRow); form->addWidget(metadata);
    m_default=new QCheckBox(tr("Default for sessions started outside Zerus"));m_default->setObjectName("defaultProject");form->addWidget(m_default);
    connect(m_default,&QCheckBox::clicked,this,[this](bool checked){if(checked){m_projects->setDefaultProject(currentId());changed();}else showProject();});
    m_folderSplitter=new FolderSplitter(Qt::Vertical);m_folderSplitter->setObjectName("projectFolderSplitter");
    m_folderSplitter->setChildrenCollapsible(false);m_folderSplitter->setHandleWidth(16);
    form->addWidget(m_folderSplitter,1);
    auto *folderPane=new QWidget;auto *folderLayout=new QVBoxLayout(folderPane);folderLayout->setContentsMargins(0,0,0,0);folderLayout->setSpacing(8);
    m_folderSplitter->addWidget(folderPane);
    auto *foldersLabel = new QLabel(tr("Folders")); foldersLabel->setObjectName("heading");
    m_newSession=new QPushButton(tr("New session…"));m_newSession->setObjectName("newProjectFolderSession");m_newSession->setAutoDefault(false);
    auto *foldersHeading=new QHBoxLayout;foldersHeading->addWidget(foldersLabel);foldersHeading->addStretch();foldersHeading->addWidget(m_newSession);folderLayout->addLayout(foldersHeading);
    m_folders = new QTableWidget; m_folders->setObjectName("projectFolders"); m_folders->setColumnCount(3);
    m_folders->setHorizontalHeaderLabels({tr("Name"),tr("Computer"),tr("Folder")});
    m_folders->setSortingEnabled(true);m_folders->sortItems(0,Qt::AscendingOrder);
    m_folders->horizontalHeader()->setToolTip(tr("Click a column heading to sort folders."));
    m_folders->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Interactive); m_folders->setColumnWidth(0,160);
    m_folders->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Interactive); m_folders->setColumnWidth(1,110);
    m_folders->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    m_folders->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);
    m_folders->verticalHeader()->hide(); m_folders->verticalHeader()->setDefaultSectionSize(38); m_folders->setShowGrid(false);
    m_folders->setSelectionBehavior(QAbstractItemView::SelectRows); m_folders->setSelectionMode(QAbstractItemView::SingleSelection);
    m_folders->setWordWrap(false); m_folders->setTextElideMode(Qt::ElideMiddle);
    m_folders->setItemDelegateForColumn(1,new FolderMachineDelegate(m_folders));
    m_folders->setEditTriggers(QAbstractItemView::NoEditTriggers); folderLayout->addWidget(m_folders,1);
    auto *folderActions = new QHBoxLayout; auto *addFolderButton = new QPushButton(tr("Add folder…")); addFolderButton->setObjectName("addProjectFolder");
    m_openFolder = new QPushButton(tr("Open folder")); m_openFolder->setObjectName("openProjectFolder");
    m_editFolder = new QPushButton(tr("Edit folder…")); m_editFolder->setObjectName("editProjectFolder");
    m_removeFolder = new QPushButton(tr("Remove folder")); m_removeFolder->setObjectName("removeProjectFolder");
    m_removeFolder->setToolTip(tr("Remove this folder from the project. Files stay in place."));
    folderActions->addWidget(addFolderButton); folderActions->addWidget(m_editFolder); folderActions->addWidget(m_removeFolder); folderActions->addStretch(); folderActions->addWidget(m_openFolder); folderLayout->addLayout(folderActions);
    m_folders->setMinimumHeight(140);
    m_worktrees=new WorktreePanel(m_client,false,nullptr,WorktreePanel::Layout::Compact);m_folderSplitter->addWidget(m_worktrees);
    // The entire gap belongs to the handle, so its line sits between both action rows.
    m_worktrees->layout()->setContentsMargins(0,0,0,0);
    m_folderSplitter->setStretchFactor(0,1);m_folderSplitter->setStretchFactor(1,0);
    m_folderSplitter->handle(1)->setToolTip(tr("Drag to resize folders and worktrees."));
    connect(m_folderSplitter,&QSplitter::splitterMoved,this,[this]{
        if(m_folderSplitInitialized&&m_worktrees->isVisible())QSettings().setValue("workspace/projectFolderSplit",m_folderSplitter->saveState());
    });
    m_worktrees->filterRequested=[this](const QString &path,bool checkout){const auto folder=selectedFolder();emit folderSessionsRequested(folder.machine==m_fleet.local().host?QString():folder.machine,path,checkout);};
    m_worktrees->sessionRequested=[this](const QString &name,const QString &archive){const auto folder=selectedFolder();emit relatedSessionRequested(folder.machine==m_fleet.local().host?QString():folder.machine,name,archive);};
    m_worktrees->launchRequested=[this](const QString &path){const auto folder=selectedFolder();if(folderLaunchProblem(folder).isEmpty())emit newPathSessionRequested(currentId(),folder.machine==m_fleet.local().host?QString():folder.machine,path);};
    auto *foot = new QHBoxLayout; m_remove = new QPushButton(tr("Delete project…")); foot->addWidget(m_remove); foot->addStretch(); form->addLayout(foot); split->addWidget(right); split->setSizes({230,600});
    m_status = new QLabel; m_status->setObjectName("projectsStatus"); m_status->setTextFormat(Qt::PlainText); m_status->setMinimumHeight(22); root->addWidget(m_status);
    if (!embedded) { auto *close = new QDialogButtonBox(QDialogButtonBox::Close); connect(close,&QDialogButtonBox::rejected,this,&QDialog::accept); root->addWidget(close); }
    connect(add,&QPushButton::clicked,this,&ProjectsDialog::addProject);
    connect(m_remove,&QPushButton::clicked,this,&ProjectsDialog::removeProject);
    connect(addFolderButton,&QPushButton::clicked,this,&ProjectsDialog::addFolder);
    connect(m_openFolder,&QPushButton::clicked,this,[this]{openFolder(selectedFolder());});
    connect(m_newSession,&QPushButton::clicked,this,[this]{newFolderSession(currentId(),selectedFolder());});
    connect(m_removeFolder,&QPushButton::clicked,this,[this]{removeFolder(currentId(),selectedFolder().id);});
    connect(m_editFolder,&QPushButton::clicked,this,[this]{const int row=m_folders->currentRow();if(row>=0)editFolder(m_folders->item(row,0)->data(Qt::UserRole).toString());});
    connect(m_folders,&QTableWidget::cellDoubleClicked,this,[this](int row,int){editFolder(m_folders->item(row,0)->data(Qt::UserRole).toString());});
    connect(m_list,&QListWidget::currentRowChanged,this,&ProjectsDialog::showProject);
    connect(m_folders,&QTableWidget::itemSelectionChanged,this,&ProjectsDialog::updateFolderActions);
    m_folders->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_folders,&QTableWidget::customContextMenuRequested,this,&ProjectsDialog::showFolderMenu);
    connect(m_name,&QLineEdit::textEdited,this,[this]{emit catalogDraftEditing(true);});
    connect(m_name,&QLineEdit::editingFinished,this,[this]{
        if (m_name->text().trimmed().isEmpty()) showProject();
        else {m_projects->renameGroup(currentId(),m_name->text());changed();}
        emit catalogDraftEditing(false);
    });
    connect(m_color,&QPushButton::clicked,this,&ProjectsDialog::chooseColor);
    connect(m_client,&HgsClient::projectsForHostReady,this,&ProjectsDialog::importLegacy);
    connect(m_client,&HgsClient::projectsFailed,this,[this](const QString &host,const QString &){m_importing.remove(host);});
    refresh();
}
QString ProjectsDialog::currentId() const { return m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString(); }
void ProjectsDialog::setSwarmStatus(const QString &text,bool error)
{
    m_swarmStatus->setText(text);m_swarmStatus->setToolTip(text);
    m_swarmStatus->setStyleSheet(error?QStringLiteral("color: #db9f42;"):QString());
}
void ProjectsDialog::refresh()
{
    if (m_worktreeOverview) m_worktreeOverview->setContext(*m_projects, m_fleet, m_allProjects->isChecked());
    const auto selected = currentId(); const QSignalBlocker block(m_list); m_list->clear();
    for (const auto &project : m_projects->groups()) {
        if(!project.accessible&&!m_allProjects->isChecked())continue;
        auto *item = new QListWidgetItem(workspaceIcon("projects",QColor(project.color)),project.name,m_list); item->setData(Qt::UserRole,project.id); item->setData(Qt::UserRole+1,QColor(project.color)); item->setData(Qt::UserRole+2,project.vivid);
        const bool isDefault=project.id==m_projects->defaultProject();item->setData(Qt::UserRole+3,isDefault);
        item->setFlags((item->flags()|Qt::ItemIsDragEnabled)&~Qt::ItemIsDropEnabled);
        const auto description=isDefault?tr("%1, default project for sessions started outside Zerus").arg(project.name):project.name;
        item->setData(Qt::AccessibleTextRole,description);item->setToolTip(description+tr("\nDrag to reorder, or use Alt+Up / Alt+Down."));
        if (project.id == selected) m_list->setCurrentItem(item);
    }
    if (!m_list->currentItem() && m_list->count()) m_list->setCurrentRow(0);
    showProject();
}
void ProjectsDialog::moveProject(int offset)
{
    const auto groups=m_projects->groups();const auto id=currentId();int index=0;
    while(index<groups.size()&&groups[index].id!=id)++index;
    if(index>=groups.size()||index+offset<0||index+offset>=groups.size())return;
    const auto before=offset<0?groups[index-1].id:index+2<groups.size()?groups[index+2].id:QString();
    m_projects->moveGroup(id,before);changed();
}
void ProjectsDialog::showProject()
{
    m_moveUp->setEnabled(m_list->currentRow()>0);m_moveDown->setEnabled(m_list->currentRow()>=0&&m_list->currentRow()+1<m_list->count());
    const auto *project = m_projects->group(currentId()); if (!project) return;
    m_name->setText(project->name); m_name->setEnabled(true); m_id->setText(project->id);
    m_preview->setProject(project->name,QColor(project->color),project->vivid,project->id==m_projects->defaultProject());
    m_default->setChecked(project->id==m_projects->defaultProject());
    const auto selected=selectedFolder().id;const QSignalBlocker blockFolders(m_folders);
    m_remove->setEnabled(m_projects->groups().size()>1);m_folders->setSortingEnabled(false); m_folders->setRowCount(0);
    for (const auto &folder : project->folders) {
        const int row = m_folders->rowCount(); m_folders->insertRow(row);
        const auto style = folderStyle(folder.machine); const auto path = MachinePlatform::native(folder.path, style);
        const QString name = folder.name.isEmpty() ? suggestName(folder.path, style) : folder.name;
        m_folders->setItem(row,0,new FolderSortItem(name)); m_folders->item(row,0)->setData(Qt::UserRole,folder.id);
        m_folders->item(row,0)->setToolTip(name);
        m_folders->setItem(row,1,new FolderSortItem(folder.machineName.isEmpty()?folder.machine:folder.machineName)); m_folders->setItem(row,2,new FolderSortItem(path));
        for(int column=1;column<3;++column)m_folders->item(row,column)->setData(Qt::UserRole,folder.id);
        m_folders->item(row,2)->setToolTip(path);
    }
    m_folders->setSortingEnabled(true);
    for(int row=0;row<m_folders->rowCount();++row)if(m_folders->item(row,0)->data(Qt::UserRole).toString()==selected){m_folders->setCurrentCell(row,0);break;}
    updateFolderActions();
}
void ProjectsDialog::changed()
{
    if (m_projects == &m_ownedProjects) QSettings().setValue("workspace/organization",QJsonDocument(m_projects->toJson()).toJson(QJsonDocument::Compact));
    emit organizationChanged(); refresh(); m_status->setText(tr("Saved"));
}
void ProjectsDialog::selectProject(const QString &id)
{
    refresh(); for (int i=0;i<m_list->count();++i) if (m_list->item(i)->data(Qt::UserRole).toString()==id) {m_list->setCurrentRow(i);break;}
}
void ProjectsDialog::selectHost(const QString &host) { m_host=host; refresh(); }
void ProjectsDialog::setFleet(const FleetState &fleet)
{
    m_fleet=fleet;
    if (m_worktreeOverview) m_worktreeOverview->setContext(*m_projects, m_fleet, m_allProjects->isChecked());
    updateFolderActions();
    const auto request=[this](const QString &host,const QString &machine,bool online){
        if (!online || machine.isEmpty() || m_projects->hasImported(machine) || m_importing.contains(host)
            || m_importDeferred.contains(machine)) return;
        m_importing.insert(host);m_client->requestProjects(host);
    };
    // A control-only computer (hgs.exe on Windows) has no project folders of its own.
    if (fleet.local().localSessions) request({},fleet.local().host,fleet.local().ok);
    for (const auto &host:fleet.peerNames()) request(host,host,fleet.peer(host)->ok);
}
void ProjectsDialog::importLegacy(const QString &host,const QList<ProjectInfo> &projects)
{
    if (!m_importing.remove(host)) return;
    const auto machine=host.isEmpty()?m_fleet.local().host:host;
    if (machine.isEmpty() || m_projects->hasImported(machine)) return;
    const auto style=folderStyle(machine);
    QStringList rejected;
    for (const auto &legacy:projects) {
        if (legacy.fromAnsible) continue;
        // A folder this computer's form does not take never creates its project.
        if (!MachinePlatform::absolute(legacy.dir,style) || legacy.dir.contains('\n')) {rejected << legacy.name+" ("+legacy.dir+")";continue;}
        QString id; for(const auto &project:m_projects->groups()) if(project.name==legacy.name){id=project.id;break;}
        if(id.isEmpty()) id=m_projects->createGroup(legacy.name);
        if(m_projects->addFolder(id,machine,legacy.dir,{},style).isEmpty()) rejected << legacy.name+" ("+legacy.dir+")";
    }
    if (rejected.isEmpty()) m_projects->markImported(machine);
    else m_importDeferred.insert(machine);
    changed();
    if (!rejected.isEmpty())
        m_status->setText(tr("Could not add %n hgs project folder(s) of %1: %2","",rejected.size()).arg(machine,rejected.join(QStringLiteral(", "))));
}
void ProjectsDialog::setProjects(const QList<ProjectInfo> &projects) { importLegacy(m_host,projects); }
void ProjectsDialog::addProject()
{
    bool ok=false;const auto name=QInputDialog::getText(this,tr("New project"),tr("Name"),QLineEdit::Normal,{},&ok);
    if(!ok || name.trimmed().isEmpty())return;
    const auto id=m_projects->createGroup(name);changed();selectProject(id);
}
void ProjectsDialog::removeProject() { deleteProject(currentId()); }
void ProjectsDialog::deleteProject(const QString &id)
{
    if(!m_projects->group(id)||m_projects->groups().size()<2)return;
    QDialog dialog(this);dialog.setWindowTitle(tr("Delete project"));dialog.setObjectName("deleteProjectDialog");dialog.setMinimumWidth(480);
    auto *layout=new QVBoxLayout(&dialog);layout->setContentsMargins(24,22,24,20);layout->setSpacing(18);
    auto *hint=new QLabel(tr("Delete “%1”? Choose the project that will receive its sessions. Files and running agents stay in place.").arg(m_projects->group(id)->name));hint->setTextFormat(Qt::PlainText);hint->setWordWrap(true);hint->setMinimumWidth(360);layout->addWidget(hint);
    auto *target=new QComboBox;target->setObjectName("projectTransferTarget");
    for(const auto &p:m_projects->groups())if(p.id!=id)target->addItem(p.name,p.id);
    const int defaultRow=target->findData(m_projects->defaultProject());if(defaultRow>=0)target->setCurrentIndex(defaultRow);layout->addWidget(target);
    layout->addSpacing(4);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Move sessions and delete"));layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    if(m_projects->removeGroup(id,target->currentData().toString()))changed();
}
void ProjectsDialog::addFolder() { editFolder({}); }
void ProjectsDialog::editFolder(const QString &id, const QString &projectId)
{
    const auto project=projectId.isEmpty()?currentId():projectId;if(!m_projects->group(project))return;
    SessionOrganization::Folder existing;
    for(const auto &folder:m_projects->group(project)->folders)if(folder.id==id)existing=folder;
    if(!id.isEmpty()&&existing.id.isEmpty())return;
    QDialog dialog(this);dialog.setObjectName("projectFolderDialog");dialog.setWindowTitle(id.isEmpty()?tr("Add project folder"):tr("Edit project folder"));dialog.setMinimumWidth(540);
    auto *layout=new QVBoxLayout(&dialog);auto *form=new QFormLayout;
    auto *machine=new QComboBox;machine->setObjectName("projectFolderMachine");
    const auto local=m_fleet.local().host;
    if(!local.isEmpty()&&m_fleet.local().localSessions)machine->addItem(local+tr(" (this computer)"),QString());
    for(const auto &peer:m_fleet.peerNames())machine->addItem(peer,peer);
    const auto initialHost=existing.id.isEmpty()?m_host:existing.machine==local?QString():existing.machine;
    if(!initialHost.isEmpty()&&machine->findData(initialHost)<0)machine->addItem(initialHost,initialHost);
    machine->setCurrentIndex(qMax(0,machine->findData(initialHost)));
    auto *name=new QLineEdit;name->setObjectName("projectFolderName");name->setPlaceholderText(tr("Optional label"));name->setText(existing.name);
    auto *path=new QLineEdit;path->setObjectName("projectFolderPath");path->setPlaceholderText(tr("Absolute folder path"));path->setText(MachinePlatform::native(existing.path,folderStyle(existing.machine)));
    auto *row=new QHBoxLayout;row->addWidget(path,1);auto *browse=new QPushButton(tr("Browse…"));row->addWidget(browse);
    form->addRow(tr("Computer"),machine);form->addRow(tr("Name"),name);form->addRow(tr("Folder"),row);layout->addLayout(form);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(id.isEmpty()?tr("Add folder"):tr("Save folder"));layout->addWidget(buttons);
    const auto validate=[&]{
        const auto host=machine->currentData().toString();const auto address=host.isEmpty()?local:host;
        // Drive and UNC folders only for a local Windows machine (D16); peers are POSIX.
        const auto style=folderStyle(address);
        bool duplicate=false;for(const auto &folder:m_projects->group(project)->folders)
            if(folder.id!=id&&folder.machine==address&&MachinePlatform::same(folder.path,MachinePlatform::clean(path->text(),style),style))duplicate=true;
        buttons->button(QDialogButtonBox::Ok)->setEnabled(!duplicate&&machine->currentIndex()>=0&&MachinePlatform::absolute(path->text(),style)&&!path->text().contains('\n'));
        path->setToolTip(duplicate?tr("This folder is already in this project."):QString());
    };
    connect(path,&QLineEdit::textChanged,&dialog,validate);validate();
    connect(browse,&QPushButton::clicked,&dialog,[&]{const auto chosen=DirectoryDialog::chooseDirectory(m_client->executable(),machine->currentData().toString(),path->text(),&dialog);if(!chosen.isEmpty())path->setText(chosen);});
    connect(machine,&QComboBox::currentIndexChanged,&dialog,[&]{path->clear();});
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(dialog.exec()!=QDialog::Accepted)return;
    const auto host=machine->currentData().toString();const auto address=host.isEmpty()?local:host;
    if(id.isEmpty())m_projects->addFolder(project,address,path->text(),name->text(),folderStyle(address));
    else m_projects->editFolder(project,id,address,path->text(),name->text(),folderStyle(address));changed();
}
void ProjectsDialog::removeFolder(const QString &projectId, const QString &id)
{
    if(id.isEmpty()||!m_projects->group(projectId))return;
    m_projects->removeFolder(projectId,id);changed();
}
void ProjectsDialog::transferFolder(const QString &projectId, const SessionOrganization::Folder &folder,
                                    const QString &destination, bool copy)
{
    const auto *source = m_projects->group(projectId);
    bool present = false;
    if (source) for (const auto &current : source->folders)
        if (current.id == folder.id && current.machine == folder.machine && current.path == folder.path
            && current.name == folder.name && current.machineId == folder.machineId && current.machineName == folder.machineName) {
            present = true;
            break;
        }
    if (!present) { m_status->setText(tr("This project folder changed. Select it again.")); return; }
    const auto *target = m_projects->group(destination);
    if (!target || destination == projectId || (!target->accessible && !m_allProjects->isChecked())) {
        m_status->setText(tr("The destination project is no longer available. Select it again."));
        return;
    }
    const auto name = target->name;
    const auto before = m_projects->toJson();
    const auto id = copy ? m_projects->copyFolder(projectId, folder.id, destination)
                         : m_projects->moveFolder(projectId, folder.id, destination);
    if (id.isEmpty()) { m_status->setText(tr("Could not transfer this project folder.")); return; }
    if (before == m_projects->toJson()) {
        m_status->setText(tr("This folder is already in %1.").arg(name));
        return;
    }
    changed();
    m_status->setText(copy ? tr("Folder copied to %1.").arg(name) : tr("Folder moved to %1.").arg(name));
}
MachinePlatform::Style ProjectsDialog::folderStyle(const QString &machine) const
{
    return MachinePlatform::of(m_fleet.local().metrics,!machine.isEmpty()&&machine==m_fleet.local().host);
}
SessionOrganization::Folder ProjectsDialog::selectedFolder() const
{
    if(m_folders->selectedItems().isEmpty())return {};
    const auto *item=m_folders->item(m_folders->currentRow(),0);
    const auto *project=m_projects->group(currentId());
    if(item&&project)for(const auto &folder:project->folders)if(folder.id==item->data(Qt::UserRole).toString())return folder;
    return {};
}
QString ProjectsDialog::folderOpenProblem(const SessionOrganization::Folder &folder) const
{
    if(folder.id.isEmpty())return tr("Select a folder to open.");
    if(folder.machine.isEmpty()||m_fleet.local().host.isEmpty())return tr("Waiting for this computer's identity.");
    if(folder.machine!=m_fleet.local().host)return tr("Open this folder from Zerus on %1.").arg(folder.machine);
    const QFileInfo path(folder.path);
    if(!path.isAbsolute()||!path.isDir())return tr("This folder is unavailable on this computer.");
    return {};
}
void ProjectsDialog::updateFolderActions()
{
    const auto folder=selectedFolder(); const auto problem=folderOpenProblem(folder);
    m_worktrees->setVisible(!folder.id.isEmpty());
    if(!folder.id.isEmpty()&&!m_folderSplitInitialized)QTimer::singleShot(0,this,[this]{
        if(m_folderSplitInitialized||selectedFolder().id.isEmpty())return;
        const auto saved=QSettings().value("workspace/projectFolderSplit").toByteArray();
        if(saved.isEmpty()||!m_folderSplitter->restoreState(saved)) {
            const int worktreeHeight=qMin(240,m_folderSplitter->height()/3);
            m_folderSplitter->setSizes({m_folderSplitter->height()-worktreeHeight,worktreeHeight});
        }
        m_folderSplitter->setHandleWidth(16);
        m_folderSplitInitialized=true;
    });
    m_worktrees->setContext(folder.machine==m_fleet.local().host?QString():folder.machine,folder.path,m_fleet);
    m_editFolder->setEnabled(!folder.id.isEmpty());m_removeFolder->setEnabled(!folder.id.isEmpty());
    m_openFolder->setEnabled(problem.isEmpty());
    const auto shown=MachinePlatform::native(folder.path,folderStyle(folder.machine));
    m_openFolder->setToolTip(problem.isEmpty()?tr("Open in file manager\n%1").arg(shown):problem);
    const auto launchProblem=folderLaunchProblem(folder);m_newSession->setEnabled(launchProblem.isEmpty());
    m_newSession->setToolTip(launchProblem.isEmpty()?tr("New session in %1 on %2").arg(shown,folder.machine):launchProblem);
}
QString ProjectsDialog::folderLaunchProblem(const SessionOrganization::Folder &folder) const
{
    if(folder.id.isEmpty())return tr("Select a folder for the new session.");
    if(folder.machine.isEmpty()||m_fleet.local().host.isEmpty())return tr("Waiting for this computer's identity.");
    const bool local=folder.machine==m_fleet.local().host;
    const auto *machine=local?&m_fleet.local():m_fleet.peer(folder.machine);
    if(!machine||!machine->ok)return tr("Reconnect %1 to start a session in this folder.").arg(folder.machine);
    if(!MachinePlatform::absolute(folder.path,folderStyle(folder.machine))||(local&&!QFileInfo(folder.path).isDir()))return tr("This folder is unavailable on this computer.");
    return {};
}
void ProjectsDialog::newFolderSession(const QString &projectId,const SessionOrganization::Folder &folder)
{
    const auto *project=m_projects->group(projectId);bool present=false;
    if(project)for(const auto &current:project->folders)
        if(current.id==folder.id&&current.machine==folder.machine&&current.path==folder.path){present=true;break;}
    if(!present){m_status->setText(tr("This project folder changed. Select it again."));return;}
    const auto problem=folderLaunchProblem(folder);if(!problem.isEmpty()){m_status->setText(problem);return;}
    emit newSessionRequested(projectId,folder.machine==m_fleet.local().host?QString():folder.machine,folder.id);
}
void ProjectsDialog::openFolder(const SessionOrganization::Folder &folder)
{
    const auto problem=folderOpenProblem(folder);
    if(!problem.isEmpty()){m_status->setText(problem);return;}
    if(!QDesktopServices::openUrl(QUrl::fromLocalFile(folder.path)))m_status->setText(tr("Could not open the folder in the file manager."));
}
void ProjectsDialog::showFolderMenu(const QPoint &position)
{
    const auto *item=position.x()<0?m_folders->currentItem():m_folders->itemAt(position);
    if(item)m_folders->setCurrentCell(item->row(),0);
    const auto folder=item?selectedFolder():SessionOrganization::Folder{};
    const auto project=currentId();
    auto *menu=new QMenu(this);menu->setObjectName("projectFolderMenu");menu->setToolTipsVisible(true);
    const QColor color(m_dark?"#a1adbb":"#647386");
    if(!folder.id.isEmpty()) {
        const auto shown=MachinePlatform::native(folder.path,folderStyle(folder.machine));
        auto *launch=menu->addAction(workspaceIcon("add",color),tr("New session…"));launch->setObjectName("contextNewProjectFolderSession");
        const auto launchProblem=folderLaunchProblem(folder);launch->setEnabled(launchProblem.isEmpty());launch->setToolTip(launchProblem.isEmpty()?shown:launchProblem);
        connect(launch,&QAction::triggered,this,[this,project,folder]{newFolderSession(project,folder);});
        menu->addSeparator();
        auto *open=menu->addAction(workspaceIcon("folder",color),tr("Open folder"));open->setObjectName("contextOpenProjectFolder");
        const auto problem=folderOpenProblem(folder);open->setEnabled(problem.isEmpty());open->setToolTip(problem.isEmpty()?shown:problem);
        connect(open,&QAction::triggered,this,[this,folder]{openFolder(folder);});
        auto *copy=menu->addAction(tr("Copy folder path"));copy->setObjectName("contextCopyProjectFolder");
        connect(copy,&QAction::triggered,this,[shown]{QApplication::clipboard()->setText(shown);});
        menu->addSeparator();
        for (const bool copyFolder : {false, true}) {
            auto *targets = menu->addMenu(copyFolder ? tr("Copy to") : tr("Move to"));
            targets->setObjectName(copyFolder ? "copyProjectFolderTo" : "moveProjectFolderTo");
            targets->setToolTipsVisible(true);
            for (const auto &target : m_projects->groups()) {
                if (target.id == project || (!target.accessible && !m_allProjects->isChecked())) continue;
                auto label = target.name;
                label.replace('&', "&&");
                auto *action = targets->addAction(workspaceIcon("projects", QColor(target.color)), label);
                action->setData(target.id);
                action->setToolTip(copyFolder ? tr("Add this folder to %1 on the same computer.").arg(target.name)
                                            : tr("Move this folder link to %1 on the same computer.").arg(target.name));
                connect(action, &QAction::triggered, this, [this, project, folder, destination = target.id, copyFolder] {
                    transferFolder(project, folder, destination, copyFolder);
                });
            }
            targets->setEnabled(!targets->actions().isEmpty());
        }
        menu->addSeparator();
        auto *edit=menu->addAction(tr("Edit folder…"));edit->setObjectName("contextEditProjectFolder");
        connect(edit,&QAction::triggered,this,[this,project,folder]{editFolder(folder.id,project);});
        auto *remove=menu->addAction(tr("Remove folder from project"));remove->setObjectName("contextRemoveProjectFolder");
        remove->setToolTip(tr("Files stay in place."));
        connect(remove,&QAction::triggered,this,[this,project,folder]{removeFolder(project,folder.id);});
        menu->addSeparator();
    }
    auto *add=menu->addAction(tr("Add folder…"));add->setObjectName("contextAddProjectFolder");add->setEnabled(!project.isEmpty());
    connect(add,&QAction::triggered,this,[this,project]{editFolder({},project);});
    connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);
    const auto point=position.x()<0&&item?m_folders->visualItemRect(item).center():position;
    menu->popup(m_folders->viewport()->mapToGlobal(point));
}
bool ProjectsDialog::nameProblem(const QString &name,QString *why)
{
    const bool bad=name.trimmed().isEmpty()||name.size()>80||name.contains('\n');
    if(why)*why=bad?tr("Use a name of 1–80 characters on one line."):QString();return bad;
}
QString ProjectsDialog::suggestName(const QString &dir, MachinePlatform::Style style)
{
    return MachinePlatform::leaf(MachinePlatform::clean(dir,style),style);
}

void ProjectsDialog::setTheme(bool dark)
{
    m_dark=dark;m_preview->setTheme(dark);m_list->setProperty("hgsDark",dark);m_list->viewport()->update();
    m_folders->setProperty("hgsDark",dark);m_folders->viewport()->setProperty("hgsDark",dark);m_folders->viewport()->update();
    m_openFolder->setIcon(workspaceIcon("folder",QColor(dark?"#a1adbb":"#647386")));
    m_newSession->setIcon(workspaceIcon("add",QColor(dark?"#a1adbb":"#647386")));
}
void ProjectsDialog::chooseColor()
{
    const auto id=currentId();const auto *project=m_projects->group(id);if(!project)return;
    const auto title=project->name;QColor chosen(project->color);
    QDialog dialog(this,Qt::Popup);dialog.setAttribute(Qt::WA_NoMouseReplay);dialog.setObjectName("projectColorDialog");dialog.setWindowTitle(tr("Project color"));dialog.setMinimumWidth(390);
    auto *layout=new QVBoxLayout(&dialog);layout->setContentsMargins(22,20,22,20);layout->setSpacing(16);
    auto *preview=new ProjectPreview;preview->setObjectName("projectFillPreview");preview->setTheme(m_dark);layout->addWidget(preview);
    auto *palette=new QGridLayout;palette->setSpacing(8);layout->addLayout(palette);
    const QStringList colors{"#ffcf33","#ff9234","#f85c63","#e45dbf","#a96bf5","#6677ff","#2b97ff","#23c5dd","#00aa7f","#41c86b","#a6cc38","#8796aa"};
    auto *hex=new QLineEdit(chosen.name());hex->setObjectName("projectColorHex");hex->setMaxLength(7);
    auto *style=new QComboBox;style->setObjectName("projectColorStyle");style->addItem(tr("Soft fill"),false);style->addItem(tr("Bright fill"),true);style->setCurrentIndex(style->findData(project->vivid));
    auto *form=new QFormLayout;form->addRow(tr("Color"),hex);form->addRow(tr("Style"),style);layout->addLayout(form);
    auto *custom=new QPushButton(tr("More colors…"));custom->setObjectName("projectCustomColor");layout->addWidget(custom,0,Qt::AlignLeft);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel);layout->addWidget(buttons);
    const auto update=[&]{const QColor value(hex->text());const bool valid=value.isValid()&&hex->text().size()==7&&hex->text().startsWith('#');buttons->button(QDialogButtonBox::Apply)->setEnabled(valid);if(valid){chosen=value;preview->setProject(title,chosen,style->currentData().toBool(),id==m_projects->defaultProject());}};
    for(int i=0;i<colors.size();++i){auto *swatch=new QPushButton;swatch->setFixedSize(44,32);swatch->setAccessibleName(colors[i]);swatch->setToolTip(colors[i]);swatch->setStyleSheet(QString("QPushButton {background:%1;border:1px solid %2;border-radius:6px;padding:0;} QPushButton:hover {border:2px solid %2;}").arg(colors[i],m_dark?"#ffffff":"#17212b"));palette->addWidget(swatch,i/6,i%6);connect(swatch,&QPushButton::clicked,&dialog,[=]{hex->setText(colors[i]);});}
    connect(hex,&QLineEdit::textChanged,&dialog,update);connect(style,&QComboBox::currentIndexChanged,&dialog,update);update();
    connect(custom,&QPushButton::clicked,&dialog,[&]{WorkspaceColorDialog picker(chosen,&dialog);connect(&picker,&QColorDialog::currentColorChanged,&dialog,[&](const QColor &c){preview->setProject(title,c,style->currentData().toBool(),id==m_projects->defaultProject());});if(picker.exec()==QDialog::Accepted)hex->setText(picker.selectedColor().name());update();});
    connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    if(execWorkspaceColorPopup(dialog)!=QDialog::Accepted)return;
    m_projects->setColor(id,chosen.name());m_projects->setVivid(id,style->currentData().toBool());changed();
}
