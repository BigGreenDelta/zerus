#include "NewSessionDialog.h"
#include "WorkspaceIcons.h"
#include "DirectoryDialog.h"
#include "WorktreePanel.h"
#include "NewWorktreeDialog.h"
#include "SessionTag.h"
#include "AccountUsage.h"
#include "AccountUsageStore.h"
#include "AccountCatalog.h"
#include "WorkspaceList.h"
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QToolButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QUuid>
#include <QVBoxLayout>
namespace {
enum { ProviderRole=Qt::UserRole+1, ProfileRole, UsageTextRole, UsageColorRole };
class LaunchAccountDelegate final : public QStyledItemDelegate {
public:
    explicit LaunchAccountDelegate(QWidget *parent):QStyledItemDelegate(parent),m_limits(new AccountUsage::Button(parent)) {
        m_limits->setObjectName("launchAccountLimitsRenderer");m_limits->hide();
        m_limits->setShowProvider(false);m_limits->setCompact(true);m_limits->setFocusPolicy(Qt::NoFocus);
        m_limits->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        return {260,16+option.fontMetrics.lineSpacing()*2};
    }
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        auto background=option;initStyleOption(&background,index);background.text.clear();background.icon={};
        const auto *widget=option.widget;auto *style=widget?widget->style():QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem,&background,p,widget);
        p->save();p->setClipRect(option.rect);
        const bool selected=option.state&QStyle::State_Selected;
        const bool dark=widget&&widget->property("hgsDark").toBool();
        const QColor color(dark?"#e8edf4":"#1a2733");
        auto rect=option.rect.adjusted(10,8,-10,-8);
        const int logo=option.fontMetrics.lineSpacing()*2;
        index.data(Qt::DecorationRole).value<QIcon>().paint(p,QRect(rect.x(),rect.y(),logo,logo));rect.adjust(logo+12,0,0,0);
        auto font=option.font;font.setWeight(QFont::DemiBold);p->setFont(font);p->setPen(color);
        p->drawText(QRect(rect.x(),rect.y(),rect.width(),option.fontMetrics.lineSpacing()),Qt::AlignVCenter,
            QFontMetrics(font).elidedText(index.data().toString(),Qt::ElideRight,rect.width()));
        const QRect limits(rect.x(),rect.y()+option.fontMetrics.lineSpacing(),rect.width(),option.fontMetrics.lineSpacing());
        const auto data=AccountUsage::launchMeterData(index.data(ProfileRole).toJsonObject()["usage"].toObject());
        if(!data["windows"].toArray().isEmpty()){
            // Render the actual status-footer widget without its provider badge.
            // It remains hidden and never receives input or fetches account data.
            m_limits->setTheme(dark);m_limits->setFont(option.font);m_limits->setIndicator(AccountUsage::indicator());m_limits->setData(data);
            p->setClipRect(limits,Qt::IntersectClip);
            m_limits->paintContents(*p,QPoint(limits.x()-4,limits.y()+(limits.height()-m_limits->height())/2));
        }else{
            const auto usageColor=index.data(UsageColorRole).value<QColor>();
            p->setFont(option.font);p->setPen(usageColor.isValid()?usageColor:selected?color:QColor(dark?"#a1adbb":"#647386"));
            p->drawText(limits,Qt::AlignVCenter,option.fontMetrics.elidedText(index.data(UsageTextRole).toString(),Qt::ElideRight,limits.width()));
        }
        p->restore();
    }
private:
    AccountUsage::Button *m_limits;
};
QLabel *plainLabel(const QString &text = {}) {auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);return l;}
bool withinFolder(const QString &path,const QString &folder) {
    return QDir::isAbsolutePath(folder)&&(path==folder||path.startsWith(folder=="/"?folder:folder+'/'));
}
}
NewSessionDialog::NewSessionDialog(const QString &hgsPath,const FleetState &fleet,const QString &selectedHost,
    const QString &agent,const QString &project,QWidget *parent,AccountUsageStore *usage)
    : QDialog(parent),m_client(hgsPath,this),m_fleet(fleet),m_initialProject(project),m_initialHost(selectedHost)
{
    m_usageStore=usage?usage:new AccountUsageStore(hgsPath,this);m_usageStore->setFleet(fleet);
    setObjectName("newSessionDialog");setWindowTitle(tr("New session"));resize(660,730);setMinimumWidth(520);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(28,24,28,24);layout->setSpacing(18);
    auto *heading=plainLabel(tr("New session"));heading->setObjectName("heading");layout->addWidget(heading);
    auto *form=new QFormLayout;form->setSpacing(12);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_project=new QComboBox;m_project->setObjectName("launchProject");m_project->setAccessibleName(tr("Project"));
    const auto theme=QSettings().value("workspace/theme","system").toString();
    const bool dark=theme=="dark"||(theme!="light"&&palette().color(QPalette::Window).lightness()<128);
    const QColor iconColor(dark?"#a1adbb":"#647386");
    m_generatedBranch="work-"+QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_manage=new QToolButton;m_manage->setObjectName("manageLaunchProject");
    m_manage->setAutoRaise(true);m_manage->setIcon(workspaceIcon("settings",iconColor));m_manage->setIconSize(QSize(18,18));m_manage->setFixedSize(34,34);
    m_manage->setToolTip(tr("Manage this project's folders"));m_manage->setAccessibleName(m_manage->toolTip());
    auto *projectRow=new QHBoxLayout;projectRow->setSpacing(8);projectRow->addWidget(m_project,1);projectRow->addWidget(m_manage);
    form->addRow(tr("Project"),projectRow);
    m_machine=new QComboBox;m_machine->setObjectName("launchComputer");m_machine->setAccessibleName(tr("Computer"));form->addRow(tr("Computer"),m_machine);
    m_folder=new QComboBox;m_folder->setObjectName("launchProjectFolder");m_folder->setAccessibleName(tr("Working folder"));
    m_folder->setMinimumContentsLength(16);m_folder->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_folder->setPlaceholderText(tr("Choose a folder…"));
    m_browse=new QPushButton(tr("Browse…"));m_browse->setObjectName("browseLaunchFolder");m_browse->setAutoDefault(false);
    auto *folderRow=new QHBoxLayout;folderRow->setSpacing(8);folderRow->addWidget(m_folder,1);folderRow->addWidget(m_browse);
    auto *folderField=new QVBoxLayout;folderField->setSpacing(6);folderField->addLayout(folderRow);
    m_projectPath=plainLabel();m_projectPath->setObjectName("launchFolderPath");m_projectPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_projectPath->setWordWrap(false);
    m_projectPath->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);folderField->addWidget(m_projectPath);
    m_preview=plainLabel();m_preview->setObjectName("muted");folderField->addWidget(m_preview);
    // Keep the form stable as paths and repository discovery change.
    for(auto *widget:QList<QWidget *>{m_projectPath}) {
        auto policy=widget->sizePolicy();policy.setRetainSizeWhenHidden(true);widget->setSizePolicy(policy);
    }
    form->addRow(tr("Working folder"),folderField);
    auto *worktreeField=new QVBoxLayout;worktreeField->setSpacing(6);
    auto *worktreeRow=new QHBoxLayout;worktreeRow->setSpacing(8);
    m_autoWorktree=new QPushButton(workspaceIcon("git-branch",iconColor),tr("Auto"));m_autoWorktree->setObjectName("automaticLaunchWorktree");
    m_plainFolder=new QPushButton(workspaceIcon("folder",iconColor),tr("None"));m_plainFolder->setObjectName("launchWithoutWorktree");
    auto *modes=new QButtonGroup(this);
    for(auto *button:{m_autoWorktree,m_plainFolder}){
        button->setCheckable(true);button->setAutoDefault(false);modes->addButton(button);worktreeRow->addWidget(button);
        button->setStyleSheet(QString("QPushButton:checked { border-color:%1; color:%1; }").arg(dark?"#8bdfc0":"#167357"));
    }
    m_autoWorktree->setToolTip(tr("Create a generated branch and worktree when you start the session."));
    m_plainFolder->setToolTip(tr("Start directly in the working folder."));
    m_worktreeOptions=new QPushButton;m_worktreeOptions->setObjectName("launchWorktreeOptions");m_worktreeOptions->setProperty("sessionHeaderAction",true);
    configureWorkspaceIconButton(m_worktreeOptions,"more",tr("Worktree options"),dark);m_worktreeOptions->setAutoDefault(false);
    auto *worktreeMenu=new QMenu(m_worktreeOptions);
    m_worktrees=worktreeMenu->addAction(workspaceIcon("git-branch",iconColor),tr("Choose existing…"));m_worktrees->setObjectName("chooseLaunchWorktree");
    m_newWorktree=worktreeMenu->addAction(workspaceIcon("add",iconColor),tr("Create manually…"));m_newWorktree->setObjectName("newLaunchWorktree");
    m_worktreeOptions->setMenu(worktreeMenu);worktreeRow->addWidget(m_worktreeOptions);worktreeRow->addStretch();worktreeField->addLayout(worktreeRow);
    m_worktreeTarget=plainLabel();m_worktreeTarget->setObjectName("launchWorktreeTarget");m_worktreeTarget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    worktreeField->addWidget(m_worktreeTarget);form->addRow(tr("Worktree"),worktreeField);
    connect(m_autoWorktree,&QPushButton::clicked,this,[this]{m_autoPreferred=true;updateForm();});
    connect(m_plainFolder,&QPushButton::clicked,this,[this]{m_autoPreferred=false;updateForm();});
    connect(m_worktrees,&QAction::triggered,this,&NewSessionDialog::chooseWorktree);
    connect(m_newWorktree,&QAction::triggered,this,&NewSessionDialog::createWorktree);
    m_agents={"codex","claude","kimi","dsh","opencode"};
    if(!agent.isEmpty()&&agent!="sh"&&!m_agents.contains(agent))m_agents.append(agent);
    const auto lastAgent=QSettings().value("workspace/launchAgent").toString();
    m_preferredAgent=!agent.isEmpty()?agent:m_agents.contains(lastAgent)||lastAgent=="sh"?lastAgent:QString("codex");
    m_account=new WorkspaceList;m_account->setObjectName("launchAccount");m_account->setAccessibleName(tr("Account and agent"));
    m_account->setProperty("hgsDark",dark);
    m_account->setItemDelegate(new LaunchAccountDelegate(m_account));m_account->setMinimumHeight(180);m_account->setMaximumHeight(250);
    m_account->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);m_account->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *accountField=new QVBoxLayout;accountField->setSpacing(6);accountField->addWidget(m_account);
    auto *accountStatusRow=new QHBoxLayout;
    m_accountStatus=plainLabel();m_accountStatus->setObjectName("launchAccountStatus");accountStatusRow->addWidget(m_accountStatus,1);
    m_refreshAccounts=new QPushButton(workspaceIcon("refresh",iconColor),tr("Refresh"));m_refreshAccounts->setObjectName("refreshLaunchAccounts");m_refreshAccounts->setAutoDefault(false);
    m_refreshAccounts->setAccessibleName(tr("Refresh accounts"));accountStatusRow->addWidget(m_refreshAccounts);
    accountField->addLayout(accountStatusRow);form->addRow(tr("Account and agent"),accountField);
    m_model=new QComboBox;m_model->setObjectName("launchModel");m_model->setAccessibleName(tr("Model"));m_model->setMinimumContentsLength(18);m_model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelLabel=plainLabel(tr("Model"));form->addRow(m_modelLabel,m_model);
    m_model->addItem(tr("Native default model"),QString());m_model->hide();m_modelLabel->hide();
    m_name=new QLineEdit(m_generatedBranch);m_name->setObjectName("launchName");form->addRow(tr("Session name"),m_name);
    m_nameError=plainLabel();m_nameError->setObjectName("launchNameError");m_nameError->hide();form->addRow(QString(),m_nameError);
    m_openTerminal=new QCheckBox(tr("Open terminal window"));m_openTerminal->setObjectName("launchOpenTerminal");
    m_openTerminal->setChecked(QSettings().value("workspace/launchOpenTerminal",false).toBool());form->addRow(QString(),m_openTerminal);
    layout->addLayout(form);
    m_error=plainLabel();m_error->setObjectName("launchError");layout->addWidget(m_error);
    layout->addStretch();auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);m_cancel=buttons->button(QDialogButtonBox::Cancel);m_start=buttons->addButton(tr("Start session"),QDialogButtonBox::AcceptRole);m_start->setObjectName("primary");layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(m_start,&QPushButton::clicked,this,&NewSessionDialog::start);
    connect(m_manage,&QToolButton::clicked,this,[this]{const auto id=m_project->currentData().toString();reject();emit projectFoldersRequested(id);});
    connect(m_project,&QComboBox::currentIndexChanged,this,[this]{m_prefillRequest=0;loadMachines();loadFolders();});
    connect(m_machine,&QComboBox::currentIndexChanged,this,[this]{m_prefillRequest=0;m_accountPreset=false;loadFolders();});
    connect(m_browse,&QPushButton::clicked,this,&NewSessionDialog::browseFolder);
    connect(m_folder,&QComboBox::currentIndexChanged,this,[this]{
        m_prefillRequest=0;
        m_browsedFolders.remove(host());
        loadAccounts();updateForm();updateWorktrees();
    });
    connect(m_folder,&QComboBox::activated,this,[this]{m_prefillRequest=0;updateForm();});
    connect(m_account,&QListWidget::currentItemChanged,this,[this]{m_preferredAgent=this->agent();m_preferredAccount=account();m_accountPreset=true;updateModels();updateForm();});
    connect(m_refreshAccounts,&QPushButton::clicked,this,[this]{loadAccounts(true);});
    connect(m_usageStore,&AccountUsageStore::changed,this,[this](const QString &){updateAccounts();});
    auto *usageTimer=new QTimer(this);usageTimer->setInterval(60000);
    connect(usageTimer,&QTimer::timeout,this,[this]{updateAccounts();m_account->viewport()->update();});usageTimer->start();
    connect(m_name,&QLineEdit::textChanged,this,&NewSessionDialog::updateForm);
    connect(&m_client,&HgsClient::directoriesReady,this,[this](quint64 request,const QString &machine,const QJsonObject &data){
        if(!m_validation||request!=m_validation||machine!=host())return;
        m_validation=0;
        const auto path=data.value("path").toString();
        if(!path.startsWith('/')) {m_error->setText(tr("The computer did not confirm this folder. Try again."));updateForm();return;}
        if(m_startWithWorktree&&!automaticWorktreeAvailable()){
            m_error->setText(tr("The repository could not be confirmed. Choose a working folder again."));updateForm();return;
        }
        // Listed folders and worktrees of them already belong to the project.
        // Decide before creation: the selection then moves to the new worktree.
        const auto addFolder=m_folder->currentData().toString().isEmpty()&&!projectWorktree(m_folder->currentData(Qt::UserRole+2).toString())?path:QString();
        if(m_startWithWorktree){
            m_creationCatalog=m_worktreeCatalog;m_creationFolder=addFolder;m_error->setText(tr("Creating worktree…"));
            m_creation=m_client.createWorktree(host(),path,m_creationCatalog["common_dir"].toString(),m_generatedBranch,"HEAD",automaticWorktreePath());updateForm();
        }else launch(path,addFolder);
    });
    connect(&m_client,&HgsClient::directoriesFailed,this,[this](quint64 request,const QString &machine,const QString &error){
        if(!m_validation||request!=m_validation||machine!=host())return;
        m_validation=0;m_error->setText(error);updateForm();
    });
    connect(&m_client,&HgsClient::accountsReady,this,[this](quint64 id,const QString &machine,const QJsonObject &data){
        if(id!=m_accountRequest||machine!=host())return;
        m_accountRequest=0;m_accountCatalogLoaded=true;m_accounts=data["profiles"].toArray();m_accountStatus->setText({});updateAccounts();
    });
    connect(&m_client,&HgsClient::accountsFailed,this,[this](quint64 id,const QString &machine,const QString &error){
        if(id!=m_accountRequest||machine!=host())return;
        m_accountRequest=0;m_accountCatalogLoaded=true;updateAccounts();m_accountStatus->setText(tr("Could not load accounts: %1").arg(error));
    });
    connect(&m_client,&HgsClient::worktreesReady,this,[this](quint64 request,const QString &machine,const QString &path,const QJsonObject &data){
        if(m_prefillRequest&&request==m_prefillRequest&&machine==host()&&path==m_prefillPath){
            m_prefillRequest=0;finishFolderPrefill(data);updateForm();return;
        }
        if(request!=m_catalogRequest||machine!=host()||path!=m_folder->currentData(Qt::UserRole+2).toString())return;
        m_catalogRequest=0;m_worktreeCatalog=data;m_worktrees->setVisible(!data["worktrees"].toArray().isEmpty());
        m_newWorktree->setVisible(data["state"]=="ok");updateForm();
    });
    connect(&m_client,&HgsClient::worktreesFailed,this,[this](quint64 request,const QString &,const QString &,const QString &){
        if(m_prefillRequest&&request==m_prefillRequest){m_prefillRequest=0;updateForm();return;}
        if(request!=m_catalogRequest)return;m_catalogRequest=0;m_worktreeCatalog["stale"]=true;updateForm();
    });
    connect(&m_client,&HgsClient::worktreeCreated,this,[this](quint64 request,bool ok,const QJsonObject &result,const QString &error){
        if(!m_creation||request!=m_creation)return;m_creation=0;
        if(!ok){m_creationFailed=true;m_error->setText(error);updateForm();return;}
        const auto path=result["path"].toString();
        auto entries=m_creationCatalog["worktrees"].toArray();entries.append(QJsonObject{{"path",path},{"kind","linked"},{"available",true}});
        m_creationCatalog["worktrees"]=entries;m_verifiedWorktrees[host()+'\n'+path]=m_creationCatalog;
        m_autoPreferred=false;selectPath(path);launch(path,m_creationFolder);
    });
    setGroups(SessionOrganization(QJsonDocument::fromJson(QSettings().value("workspace/organization").toByteArray()).object()),project);
}
void NewSessionDialog::reject(){if(!m_creation)QDialog::reject();}
QString NewSessionDialog::host() const {return m_machine->currentData().toString();}
void NewSessionDialog::setFleet(const FleetState &fleet)
{
    m_fleet=fleet;
    m_usageStore->setFleet(fleet);updateAccounts();
    if(m_creation||m_validation){updateForm();return;}
    loadMachines();loadFolders();
}
void NewSessionDialog::loadMachines()
{
    const auto previous=m_machine->currentIndex()<0?m_initialHost:host();
    // A control-only computer (hgs.exe on Windows) starts sessions on remote machines only.
    const bool local=m_fleet.local().localSessions;
    QStringList machines;if(local)machines<<m_fleet.local().host;machines.append(m_fleet.peerNames());
    if(!previous.isEmpty()&&!machines.contains(previous))machines.append(previous);
    if(const auto *project=m_projects.group(m_project->currentData().toString()))
        for(const auto &folder:project->folders)if(!machines.contains(folder.machine) && (folder.machine!=m_fleet.local().host || local) && (folder.machineId.isEmpty() || folder.machine==m_fleet.local().host || m_fleet.peerNames().contains(folder.machine)))machines.append(folder.machine);
    QList<QPair<QString,QString>> items;
    for(const auto &machine:machines){
        if(machine.isEmpty()||(!local&&machine==m_fleet.local().host))continue;
        const auto target=machine==m_fleet.local().host?QString():machine;
        const auto *box=target.isEmpty()?&m_fleet.local():m_fleet.peer(target);
        items.append({machine+(box&&box->ok?QString():tr(" (offline)")),target});
    }
    // Fleet polls refresh labels in place: new rows would reset an open list's hovered row.
    const QSignalBlocker block(m_machine);
    bool same=m_machine->count()==items.size();
    for(int row=0;same&&row<items.size();++row)same=m_machine->itemData(row).toString()==items[row].second;
    if(!same){m_machine->clear();for(const auto &item:items)m_machine->addItem(item.first,item.second);}
    else for(int row=0;row<items.size();++row)if(m_machine->itemText(row)!=items[row].first)m_machine->setItemText(row,items[row].first);
    m_machine->setCurrentIndex(qMax(0,m_machine->findData(previous)));
}

void NewSessionDialog::setGroups(const SessionOrganization &projects,const QString &selected)
{
    m_prefillRequest=0;
    m_projects=projects;const QSignalBlocker block(m_project);m_project->clear();
    for(const auto &p:m_projects.groups())if(p.accessible)m_project->addItem(workspaceIcon("projects",QColor(p.color)),p.name,p.id);
    int index=m_project->findData(selected);if(index<0)index=m_project->findText(m_initialProject);
    if(index<0)index=m_project->findData(m_projects.defaultProject());m_project->setCurrentIndex(index);loadMachines();loadFolders();
}
bool NewSessionDialog::selectFolder(const QString &folderId)
{
    m_prefillRequest=0;
    const auto *project=m_projects.group(m_project->currentData().toString());
    if(!project)return false;
    for(const auto &folder:project->folders)if(folder.id==folderId){
        const auto target=folder.machine==m_fleet.local().host?QString():folder.machine;
        const int machine=m_machine->findData(target);if(machine<0)return false;
        m_machine->setCurrentIndex(machine);
        const int index=m_folder->findData(folderId);if(index<0)return false;
        m_browsedFolders.remove(host());m_folder->setCurrentIndex(index);updateForm();return true;
    }
    return false;
}
void NewSessionDialog::loadFolders(const QString &selectedPath)
{
    const auto context=m_project->currentData().toString()+'\n'+host();
    // Refreshes retain the displayed folder, but only Browse choices cross projects.
    const auto previous=context==m_folderContext?m_folder->currentData(Qt::UserRole+2).toString():QString();
    m_folderContext=context;
    const auto *project=m_projects.group(m_project->currentData().toString());
    const auto *machine=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    const bool available=machine&&machine->ok;
    struct Row {QString id,path,name;};
    QList<Row> rows;
    const auto add=[&](const QString &id,const QString &rawPath,const QString &name){
        const auto path=QDir::cleanPath(rawPath);
        rows.append({id,path,name.isEmpty()?(path=="/"?path:path.section('/',-1)):name});
    };
    if(project)for(const auto &folder:project->folders){
        if((folder.machine==m_fleet.local().host?QString():folder.machine)!=host())continue;
        add(folder.id,folder.path,folder.name);
    }
    const auto custom=!selectedPath.isEmpty()?selectedPath:!previous.isEmpty()?previous:m_browsedFolders.value(host());
    int index=rows.isEmpty()?-1:0;
    if(!custom.isEmpty()){
        index=-1;for(int row=0;row<rows.size()&&index<0;++row)if(rows[row].path==custom)index=row;
        if(index<0){add({},custom,{});index=rows.size()-1;}
    }
    // Fleet polls refresh availability in place: new rows would reset an open
    // list's hovered row. Messages stay until the folder choice changes.
    const QSignalBlocker block(m_folder);
    bool same=m_folder->count()==rows.size();
    for(int row=0;same&&row<rows.size();++row)
        same=m_folder->itemData(row).toString()==rows[row].id&&m_folder->itemData(row,Qt::UserRole+2).toString()==rows[row].path
            &&m_folder->itemText(row)==rows[row].name&&m_folder->itemData(row,Qt::UserRole+1).toString()==host();
    if(!same||m_folder->currentIndex()!=index)m_error->clear();
    if(!same){
        m_folder->clear();
        for(const auto &row:rows){
            m_folder->addItem(row.name,row.id);const int at=m_folder->count()-1;
            m_folder->setItemData(at,host(),Qt::UserRole+1);m_folder->setItemData(at,row.path,Qt::UserRole+2);m_folder->setItemData(at,row.path,Qt::ToolTipRole);
        }
    }
    for(int row=0;row<m_folder->count();++row)m_folder->setItemData(row,available,Qt::UserRole+3);
    m_folder->setCurrentIndex(index);
    loadAccounts();updateForm();updateWorktrees();
}
void NewSessionDialog::browseFolder()
{
    const auto target=host(),project=m_project->currentData().toString();
    const auto path=DirectoryDialog::chooseDirectory(m_client.executable(),target,m_folder->currentData(Qt::UserRole+2).toString(),this);
    if(path.isEmpty()||host()!=target||m_project->currentData().toString()!=project)return;
    selectPath(path);
    m_browsedFolders[target]=QDir::cleanPath(path);
}

void NewSessionDialog::updateForm()
{
    const auto path=m_folder->currentData(Qt::UserRole+2).toString();
    m_projectPath->setText(path);m_projectPath->setVisible(!path.isEmpty());
    m_projectPath->setToolTip(path);m_projectPath->ensurePolished();m_projectPath->setFixedHeight(m_projectPath->fontMetrics().height());
    const bool worktree=m_folder->currentData().toString().isEmpty()&&projectWorktree(path);
    const bool outside=!path.isEmpty()&&m_folder->currentData().toString().isEmpty()&&!worktree;
    m_preview->setProperty("outsideProject",outside);
    m_preview->setStyleSheet(outside?QStringLiteral("color: %1;").arg(palette().color(QPalette::Window).lightness()<128?"#f0b65a":"#9b6300"):QString());
    m_preview->setText(m_prefillRequest?tr("Checking the session's main working folder…"):
        path.isEmpty()?tr("Browse to choose a folder on this computer."):
        outside?tr("This folder is outside the project.\nStarting here will add it to this project."):
        QString());
    m_preview->setToolTip(m_preview->text());m_preview->ensurePolished();m_preview->setFixedHeight(2*m_preview->fontMetrics().lineSpacing());
    const QString nameProblem=SessionTag::problem(m_name->text());const bool validName=nameProblem.isEmpty();
    m_nameError->setText(nameProblem);m_nameError->setVisible(!validName);
    const bool busy=m_validation||m_creation;
    const bool noMachine=m_machine->count()==0;
    const bool automatic=m_autoPreferred&&automaticWorktreeAvailable();
    m_start->setEnabled(!noMachine&&m_account->currentItem()&&(!m_accountRequest||agent()=="sh")&&!busy&&!m_prefillRequest&&validName&&!path.isEmpty()&&m_folder->currentData(Qt::UserRole+3).toBool()
        &&(!m_autoPreferred||!m_catalogRequest)&&!(automatic&&m_creationFailed));
    m_start->setText(m_creation?tr("Creating worktree…"):m_validation?tr("Checking folder…"):tr("Start session"));
    for(auto *widget:QList<QWidget *>{m_project,m_machine,m_folder,m_account,m_model,m_name,m_manage,m_worktreeOptions})widget->setEnabled(!busy);
    m_refreshAccounts->setEnabled(!busy&&!noMachine&&!m_accountRequest);
    m_autoWorktree->setEnabled(!busy&&automaticWorktreeAvailable());m_plainFolder->setEnabled(!busy);m_cancel->setEnabled(!m_creation);
    m_autoWorktree->setChecked(automatic);m_plainFolder->setChecked(!automatic);
    m_worktreeTarget->setText(automatic?automaticWorktreePath():QString());m_worktreeTarget->setToolTip(automatic?tr("New branch: %1\nStart from: HEAD").arg(m_generatedBranch):QString());
    m_worktreeTarget->ensurePolished();m_worktreeTarget->setFixedHeight(2*m_worktreeTarget->fontMetrics().lineSpacing());
    const auto *machine=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    m_browse->setEnabled(!busy&&!noMachine&&machine&&machine->ok);
    m_worktrees->setEnabled(!busy&&!m_catalogRequest&&!m_worktreeCatalog["stale"].toBool()&&!m_worktreeCatalog["worktrees"].toArray().isEmpty());
    m_newWorktree->setEnabled(!busy&&!m_catalogRequest&&machine&&machine->ok&&automaticWorktreeAvailable());
    m_worktreeOptions->setEnabled(m_worktrees->isEnabled()||m_newWorktree->isEnabled());
    m_openTerminal->setEnabled(!busy&&agent()!="dsh"&&agent()!="opencode");
    m_openTerminal->setToolTip(agent()=="dsh"?tr("DeepSeek opens in the workspace's Native UI."):agent()=="opencode"?tr("OpenCode runs through its native server and opens in Activity."):tr("Also open an external terminal for this session."));
    const auto offline=tr("This computer is unavailable. Choose another folder or reconnect it.");
    if(!path.isEmpty()&&!m_folder->currentData(Qt::UserRole+3).toBool())m_error->setText(offline);
    else if(m_error->text()==offline)m_error->clear();
    const auto noAccount=tr("Restore or add an account in Agent accounts to start this agent.");
    if(m_account->count()==0)m_error->setText(noAccount);else if(m_error->text()==noAccount)m_error->clear();
    const auto loadingAccount=tr("Loading this session's account…"),missingAccount=tr("This session's account is unavailable. Choose another account.");
    if(m_accountPreset&&!m_account->currentItem())m_error->setText(m_accountRequest?loadingAccount:missingAccount);
    else if(m_error->text()==loadingAccount||m_error->text()==missingAccount)m_error->clear();
    const auto remoteOnly=tr("Add a remote machine in Machines to start sessions from this computer.");
    if(noMachine)m_error->setText(remoteOnly);else if(m_error->text()==remoteOnly)m_error->clear();
}
void NewSessionDialog::start()
{
    if(!m_start->isEnabled())return;
    m_startWithWorktree=m_autoPreferred&&automaticWorktreeAvailable();
    m_error->setText(tr("Checking folder…"));m_validation=m_client.requestDirectories(host(),m_folder->currentData(Qt::UserRole+2).toString());updateForm();
}
bool NewSessionDialog::automaticWorktreeAvailable() const
{
    const auto path=m_folder->currentData(Qt::UserRole+2).toString();
    const auto root=m_worktreeCatalog["selected_root"].toString();
    if(m_catalogRequest||m_worktreeCatalog["state"]!="ok"||m_worktreeCatalog["stale"].toBool()||m_worktreeCatalog["partial"].toBool()
        ||m_worktreeCatalog["common_dir"].toString().isEmpty()||!QDir::isAbsolutePath(root)||QFileInfo(root).fileName().isEmpty())return false;
    for(const auto &value:m_worktreeCatalog["worktrees"].toArray()){
        const auto entry=value.toObject();
        if(entry["available"].toBool()&&entry["kind"]!="bare"&&withinFolder(path,entry["path"].toString()))return true;
    }
    return false;
}
QString NewSessionDialog::automaticWorktreePath() const
{
    const QFileInfo root(m_worktreeCatalog["selected_root"].toString());
    return QDir(root.path()).filePath(root.fileName()+'-'+m_generatedBranch);
}
void NewSessionDialog::launch(const QString &target,const QString &addFolder)
{
    QSettings settings;settings.setValue("workspace/launchOpenTerminal",m_openTerminal->isChecked());
    // The next dialog starts with this agent and, per machine and agent, this account.
    settings.setValue("workspace/launchAgent",agent());
    if(const auto selectedAccount=account();!selectedAccount.isEmpty()){
        auto accounts=QJsonDocument::fromJson(settings.value("workspace/launchAccounts").toByteArray()).object();
        auto machine=accounts.value(host()).toObject();machine[agent()]=selectedAccount;accounts[host()]=machine;
        settings.setValue("workspace/launchAccounts",QJsonDocument(accounts).toJson(QJsonDocument::Compact));
    }
    emit launchRequested(host(),agent(),target,m_name->text(),account(),m_project->currentData().toString(),m_openTerminal->isChecked()&&agent()!="dsh"&&agent()!="opencode",addFolder,agent()=="opencode"?m_model->currentData().toString():QString());accept();
}
QString NewSessionDialog::agent() const
{
    return m_account->currentItem()?m_account->currentItem()->data(ProviderRole).toString():m_preferredAgent;
}
QString NewSessionDialog::account() const
{
    return m_account->currentItem()?m_account->currentItem()->data(Qt::UserRole).toString():QString();
}
void NewSessionDialog::loadAccounts(bool refresh)
{
    if(m_machine->count()==0){m_accountRequest=0;m_accountCatalogLoaded=false;m_accounts={};updateAccounts();return;}
    if(m_accountHost==host()&&(m_accountRequest||(!refresh&&m_accountCatalogLoaded)))return;
    if(m_accountHost!=host()){
        m_accounts={};m_accountPreset=false;m_accountCatalogLoaded=false;
    }
    m_accountHost=host();m_accountStatus->setText(tr("Loading accounts…"));
    m_accountRequest=m_client.requestAccounts(host(),{"ls"});updateAccounts();
}
void NewSessionDialog::updateAccounts()
{
    if(m_machine->count()==0){const QSignalBlocker block(m_account);m_account->clear();updateModels();updateForm();return;}
    QJsonArray localProfiles;
    auto providers=m_agents;
    for(const auto &value:m_accounts){const auto provider=value.toObject()["provider"].toString();if(!provider.isEmpty()&&provider!="sh"&&!providers.contains(provider))providers.append(provider);}
    for(const auto &provider:providers){
        for(const auto &value:m_accounts){
            auto profile=value.toObject();if(profile["provider"]!=provider||profile["id"].toString().isEmpty())continue;
            profile["host"]=host();profile["usage"]=cachedUsage(profile);localProfiles.append(profile);
        }
    }
    // Use the Accounts page's identity/scope rules, applied only to this
    // computer's catalog. An account can have several local launch bindings.
    const auto remembered=QJsonDocument::fromJson(QSettings().value("workspace/launchAccounts").toByteArray()).object().value(host()).toObject();
    const auto current=m_account->currentItem()?m_account->currentItem()->data(ProfileRole).toJsonObject():QJsonObject();
    const auto currentId=current["host"].toString()==host()&&current["provider"]==m_preferredAgent?current["id"].toString():QString();
    QList<QJsonObject> profiles;
    for(const auto &value:AccountCatalog::group(localProfiles)){
        const auto group=value.toObject();QJsonObject chosen;int best=-1;
        for(const auto &member:group["members"].toArray()){
            const auto profile=member.toObject();const auto provider=profile["provider"].toString(),id=profile["id"].toString();
            const int rank=m_accountPreset&&provider==m_preferredAgent&&id==m_preferredAccount?5:
                provider==m_preferredAgent&&id==currentId?4:id==remembered[provider].toString()?3:
                profile["is_default"].toBool()?2:profile["native"].toBool()?1:0;
            if(rank>best){chosen=profile;best=rank;}
        }
        chosen["usage"]=group["usage"];profiles.append(chosen);
    }
    const QSignalBlocker block(m_account);
    bool same=m_account->count()==profiles.size()+1;
    for(int row=0;same&&row<profiles.size();++row)
        same=m_account->item(row)->data(Qt::UserRole).toString()==profiles[row]["id"].toString()
            &&m_account->item(row)->data(ProviderRole).toString()==profiles[row]["provider"].toString()
            &&m_account->item(row)->data(ProfileRole).toJsonObject()["host"]==profiles[row]["host"];
    if(!same)m_account->clear();
    const bool dark=m_account->property("hgsDark").toBool();
    const QColor iconColor(dark?"#a1adbb":"#647386");
    int defaultIndex=-1,lastIndex=-1,explicitIndex=-1,currentIndex=-1;
    const auto lastAccount=QJsonDocument::fromJson(QSettings().value("workspace/launchAccounts").toByteArray()).object()
        .value(host()).toObject().value(m_preferredAgent).toString();
    for(int row=0;row<profiles.size();++row){
        const auto profile=profiles[row];const auto provider=profile["provider"].toString(),id=profile["id"].toString();
        auto *item=same?m_account->item(row):new QListWidgetItem(m_account);
        item->setData(Qt::UserRole,id);item->setData(ProviderRole,provider);item->setData(ProfileRole,profile);
        item->setIcon(workspaceIcon(m_agents.contains(provider)?provider:QString("terminal"),IdentityBadges::providerColor(provider,dark)));
        if(provider==m_preferredAgent){
            if(defaultIndex<0||profile["is_default"].toBool())defaultIndex=row;
            if(id==lastAccount)lastIndex=row;
            if(id==m_preferredAccount)explicitIndex=row;
            if(id==currentId)currentIndex=row;
        }
    }
    auto *shell=same?m_account->item(profiles.size()):new QListWidgetItem(m_account);
    shell->setText(tr("Run shell"));shell->setData(Qt::UserRole,QString());shell->setData(ProviderRole,"sh");
    shell->setData(UsageTextRole,tr("Interactive terminal session"));shell->setIcon(workspaceIcon("terminal",iconColor));
    shell->setData(Qt::AccessibleTextRole,shell->text());
    if(m_preferredAgent=="sh")defaultIndex=lastIndex=explicitIndex=profiles.size();
    // Explicit selections never silently switch accounts; remembered ones may
    // fall back to the provider's default when the old profile no longer exists.
    const int selected=m_accountPreset?explicitIndex:currentIndex>=0?currentIndex:lastIndex>=0?lastIndex:defaultIndex>=0?defaultIndex:0;
    // A temporary shell-only list must not replace the intended agent while
    // its computer's authoritative account catalog is still loading.
    m_account->setCurrentRow(m_accountRequest&&profiles.isEmpty()&&m_preferredAgent!="sh"?-1:selected);
    updateUsage();updateModels();updateForm();
}

void NewSessionDialog::selectAccount(const QString &account)
{
    m_preferredAccount=account.isEmpty()?"native-"+m_preferredAgent:account;
    m_accountPreset=true;updateAccounts();
}

QJsonObject NewSessionDialog::cachedUsage(const QJsonObject &profile) const
{
    auto usage=m_usageStore->data(AccountUsageRef::profile(profile));
    const auto cached=profile["account_status"].toObject();
    if(!cached.isEmpty()&&(usage["status"]=="loading"||usage["status"]=="offline"
        ||cached["checked_at"].toDouble()>usage["checked_at"].toDouble()))usage=cached;
    return usage;
}

void NewSessionDialog::updateUsage()
{
    for(int row=0;row<m_account->count();++row){
        auto *item=m_account->item(row);const auto profile=item->data(ProfileRole).toJsonObject();
        if(profile.isEmpty())continue;
        const auto usage=profile["usage"].toObject();
        const auto cached=profile["account_status"].toObject();
        auto name=usage["identity"].toObject()["email"].toString();
        if(name.isEmpty())name=cached["identity"].toObject()["email"].toString();
        if(name.isEmpty())name=profile["label"].toString();
        if(name.isEmpty()||name=="Default account"||name=="Native account")name=tr("Account n/a");
        const auto summary=AccountUsage::launchSummary(usage);
        const auto tone=AccountUsage::severest(usage);
        const bool historical=usage["offline"].toBool()||usage["refresh_error"].toBool();
        item->setData(UsageColorRole,!historical&&tone&&*tone!=AccountUsage::Tone::Normal
            ?AccountUsage::toneColor(*tone,m_account->property("hgsDark").toBool()):QColor());
        if(item->text()!=name)item->setText(name);
        if(item->data(UsageTextRole).toString()!=summary)item->setData(UsageTextRole,summary);
        item->setToolTip(name+'\n'+summary);
        item->setData(Qt::AccessibleTextRole,IdentityBadges::providerName(profile["provider"].toString())+' '+name+'\n'+summary);
    }
}

void NewSessionDialog::selectPath(const QString &path, bool automaticWorktree)
{
    m_prefillRequest=0;
    if(!QDir::isAbsolutePath(path))return;
    if(!automaticWorktree)m_autoPreferred=false;
    m_browsedFolders.remove(host());
    loadFolders(QDir::cleanPath(path));
}
QString NewSessionDialog::matchingProjectFolder(const QString &path,const QString &canonicalPath) const
{
    QString best;
    if(const auto *project=m_projects.group(m_project->currentData().toString()))
        for(const auto &folder:project->folders){
            if((folder.machine==m_fleet.local().host?QString():folder.machine)!=host())continue;
            const auto root=QDir::cleanPath(folder.path);
            if((withinFolder(path,root)||withinFolder(canonicalPath,root))&&root.size()>best.size())best=root;
        }
    return best.isEmpty()?path:best;
}
void NewSessionDialog::prefillSessionFolder(const QString &path,const QString &canonicalPath)
{
    if(!QDir::isAbsolutePath(path))return;
    m_prefillPath=QDir::cleanPath(path);m_prefillCanonicalPath=QDir::isAbsolutePath(canonicalPath)?QDir::cleanPath(canonicalPath):QString();
    selectPath(matchingProjectFolder(m_prefillPath,m_prefillCanonicalPath));
    m_prefillContext=m_project->currentData().toString()+'\n'+host()+'\n'+m_folder->currentData(Qt::UserRole+2).toString();
    const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    if(box&&box->ok)m_prefillRequest=m_client.requestWorktrees(host(),m_prefillPath,true);
    updateForm();
}
void NewSessionDialog::finishFolderPrefill(const QJsonObject &catalog)
{
    const auto context=m_project->currentData().toString()+'\n'+host()+'\n'+m_folder->currentData(Qt::UserRole+2).toString();
    const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    if(context!=m_prefillContext||!box||!box->ok||catalog["state"]!="ok"||catalog["stale"].toBool()
        ||catalog["partial"].toBool()||catalog["common_dir"].toString().isEmpty())return;
    const auto selected=QDir::cleanPath(catalog["selected_root"].toString());
    if(!withinFolder(m_prefillPath,selected)&&!withinFolder(m_prefillCanonicalPath,selected))return;
    bool linked=false;QString main;
    for(const auto &value:catalog["worktrees"].toArray()){
        const auto entry=value.toObject();const auto root=QDir::cleanPath(entry["path"].toString());
        if(!QDir::isAbsolutePath(root)||!entry["available"].toBool())continue;
        if(root==selected&&entry["kind"]=="linked")linked=true;
        if(entry["kind"]=="main"){
            if(!main.isEmpty())return;
            main=root;
        }
    }
    if(!linked||main.isEmpty())return;
    const auto target=matchingProjectFolder(main);
    m_verifiedWorktrees[host()+'\n'+target]=catalog;
    selectPath(target);
}
void NewSessionDialog::updateWorktrees()
{
    const auto path=m_folder->currentData(Qt::UserRole+2).toString();
    const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    const auto context=host()+'\n'+path+'\n'+QString::number(box&&box->ok);
    if(context==m_worktreeContext)return;m_worktreeContext=context;m_catalogRequest=0;
    m_worktreeCatalog=m_client.worktreeSnapshot(host(),path);
    m_worktrees->setVisible(!m_worktreeCatalog["worktrees"].toArray().isEmpty());m_newWorktree->setVisible(m_worktreeCatalog["state"]=="ok");
    if(!path.isEmpty()&&box&&box->ok)m_catalogRequest=m_client.requestWorktrees(host(),path);
    updateForm();
}
bool NewSessionDialog::projectWorktree(const QString &path,const QJsonObject &catalog) const
{
    if(path.isEmpty()||catalog["state"]!="ok"||catalog["stale"].toBool()||catalog["common_dir"].toString().isEmpty())return false;
    const auto contains=[&](const QString &folder){
        for(const auto &value:catalog["worktrees"].toArray()){
            const auto entry=value.toObject();const auto root=QDir::cleanPath(entry["path"].toString());
            if(!QDir::isAbsolutePath(root)||entry["kind"]=="bare"||!entry["available"].toBool())continue;
            const auto clean=QDir::cleanPath(folder);
            if(clean==root||clean.startsWith(root=='/'?root:root+'/'))return true;
        }
        return false;
    };
    if(!contains(path))return false;
    if(const auto *project=m_projects.group(m_project->currentData().toString()))
        for(const auto &folder:project->folders)
            if((folder.machine==m_fleet.local().host?QString():folder.machine)==host()&&contains(folder.path))return true;
    return false;
}
bool NewSessionDialog::projectWorktree(const QString &path) const
{
    return projectWorktree(path,m_worktreeCatalog)||projectWorktree(path,m_verifiedWorktrees.value(host()+'\n'+path));
}
void NewSessionDialog::chooseWorktree()
{
    const auto machine=host(),project=m_project->currentData().toString();
    QDialog dialog(this);dialog.setObjectName("chooseWorktreeDialog");dialog.setWindowTitle(tr("Choose an existing worktree"));dialog.resize(620,420);
    auto *layout=new QVBoxLayout(&dialog);layout->setContentsMargins(20,18,20,18);
    auto *panel=new WorktreePanel(&m_client,true);layout->addWidget(panel);
    panel->setContext(machine,m_folder->currentData(Qt::UserRole+2).toString(),m_fleet);
    QString chosen;panel->launchRequested=[&](const QString &path){chosen=path;dialog.accept();};
    panel->cancelRequested=[&]{dialog.reject();};
    if(dialog.exec()==QDialog::Accepted&&!chosen.isEmpty()&&host()==machine&&m_project->currentData().toString()==project){m_autoPreferred=false;selectPath(chosen);}
}
void NewSessionDialog::createWorktree()
{
    if(!m_newWorktree->isEnabled())return;
    const auto machine=host(),project=m_project->currentData().toString();
    const auto source=m_folder->currentData(Qt::UserRole+2).toString();
    auto catalog=m_worktreeCatalog;
    NewWorktreeDialog dialog(&m_client,machine,machine.isEmpty()?m_fleet.local().host:machine,
        source,catalog,m_name->text(),this);
    if(dialog.exec()!=QDialog::Accepted||dialog.createdPath().isEmpty())return;
    if(host()!=machine||m_project->currentData().toString()!=project){
        m_error->setText(tr("Worktree created at %1 on %2. Select that folder to start a session.").arg(dialog.createdPath(),machine.isEmpty()?m_fleet.local().host:machine));return;
    }
    // Successful creation confirms membership even before the catalog refresh
    // includes the new checkout. Retain that evidence only in this dialog.
    auto entries=catalog["worktrees"].toArray();entries.append(QJsonObject{{"path",dialog.createdPath()},{"kind","linked"},{"available",true}});
    catalog["worktrees"]=entries;
    m_verifiedWorktrees[machine+'\n'+dialog.createdPath()]=catalog;
    m_autoPreferred=false;selectPath(dialog.createdPath());
}

void NewSessionDialog::updateModels()
{
    const bool enabled=agent()=="opencode";
    const auto selected=m_model->currentData().toString();
    const QSignalBlocker blocker(m_model);
    m_model->clear();m_model->addItem(tr("Native default model"),QString());
    if(enabled) for(const auto &value:m_accounts){
        const auto account=value.toObject();
        if(account["provider"]!="opencode"||account["id"].toString()!=this->account())continue;
        const auto options=account["model_options"].toArray();
        for(int index=0;index<qMin(256,options.size());++index){
            const auto entry=options[index];
            const auto option=entry.toObject();const auto id=option["value"].toString(option["id"].toString());
            if(id.isEmpty()||id.toUtf8().size()>256||!id.contains('/')||std::any_of(id.cbegin(),id.cend(),[](QChar c){return c.category()==QChar::Other_Control;})||m_model->findData(id)>=0)continue;
            auto label=option["label"].toString();if(label.size()>160||std::any_of(label.cbegin(),label.cend(),[](QChar c){return c.category()==QChar::Other_Control;}))label={};
            m_model->addItem(label.isEmpty()||label==id?id:label+" ("+id+")",id);
        }
    }
    m_model->setCurrentIndex(qMax(0,m_model->findData(selected)));
    m_model->setVisible(enabled);m_modelLabel->setVisible(enabled);
    m_model->setToolTip(tr("Models configured on this computer. Native default uses OpenCode's configuration."));
}
