#include "NewSessionDialog.h"
#include "RepositoryClone.h"
#include "WorkspaceIcons.h"
#include "DirectoryDialog.h"
#include "WorktreePanel.h"
#include "NewWorktreeDialog.h"
#include "SessionTag.h"
#include "AccountUsage.h"
#include "AccountUsageStore.h"
#include "AccountCatalog.h"
#include "AccountChoice.h"
#include "WorkspaceList.h"
#include "MachinePlatform.h"
#include "LocalLaunchWarning.h"
#include "OpenCodeEffort.h"
#include "OpenCodeModelPicker.h"
#include "OpenCodeModels.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QToolButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QProgressBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QUuid>
#include <QVBoxLayout>
namespace {
using namespace AccountChoice;
constexpr int NewRepositoryProjectRole=Qt::UserRole+1;
QLabel *plainLabel(const QString &text = {}) {auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);return l;}
// Each OpenCode model row keeps its validated variants for the effort choice
// and its label for the model button.
constexpr int EffortRole=Qt::UserRole+10,EffortLabelRole=Qt::UserRole+11,LabelRole=Qt::UserRole+12;
bool controlText(const QString &text) {return std::any_of(text.cbegin(),text.cend(),[](QChar c){return c.category()==QChar::Other_Control;});}
bool readyCatalog(const QJsonObject &catalog) {return catalog["catalog_state"]=="ready";}
// Valid OpenCode model entries (value, else id) the list does not have yet, at
// most 256 per call, each with its valid and distinct variants.
void addModelOptions(QComboBox *model,const QJsonArray &options)
{
    for(int index=0;index<qMin(256,options.size());++index){
        const auto entry=options[index];
        const auto option=entry.toObject();const auto id=option["value"].toString(option["id"].toString());
        if(id.isEmpty()||id.toUtf8().size()>256||!id.contains('/')||controlText(id)||model->findData(id)>=0)continue;
        auto label=option["label"].toString();if(label.size()>160||controlText(label))label={};
        // Effort options are this model's variants: at most 16, each valid and distinct.
        QStringList efforts,effortLabels;
        const auto variants=option["effort_options"].toArray(),variantLabels=option["effort_labels"].toArray();
        for(int variant=0;variant<qMin<qsizetype>(64,variants.size())&&efforts.size()<16;++variant){
            const auto effort=variants[variant].toString();
            if(!OpenCodeEffort::valid(effort)||efforts.contains(effort))continue;
            auto effortLabel=variantLabels.at(variant).toString();if(effortLabel.isEmpty()||effortLabel.size()>80||controlText(effortLabel))effortLabel=effort;
            efforts<<effort;effortLabels<<effortLabel;
        }
        model->addItem(label.isEmpty()||label==id?id:label+" ("+id+")",id);
        model->setItemData(model->count()-1,efforts,EffortRole);model->setItemData(model->count()-1,effortLabels,EffortLabelRole);
        model->setItemData(model->count()-1,label.isEmpty()?id:label,LabelRole);
    }
}
// A confirmed favorite change in model entries (value, else id): the mark is set, or
// removed as hgs leaves it out; every other field and entry stays as listed.
QJsonArray markFavorite(QJsonArray entries,const QString &value,bool favorite)
{
    for(qsizetype index=0;index<entries.size();++index){
        auto entry=entries.at(index).toObject();
        if(entry.value("value").toString(entry.value("id").toString())!=value)continue;
        if(favorite)entry.insert("favorite",true);else entry.remove("favorite");
        entries.replace(index,entry);
    }
    return entries;
}
// The same change in a ready catalog: both model lists and its favorites, where a new
// favorite is listed once, as hgs lists it ({value, available}).
void markCatalogFavorite(QJsonObject &catalog,const QString &value,bool favorite)
{
    for(const auto *list:{"model_options","models"})
        if(catalog.contains(list))catalog.insert(list,markFavorite(catalog.value(list).toArray(),value,favorite));
    if(!catalog.contains("favorites"))return;
    QJsonArray favorites;bool listed=false;
    for(const auto &entry:catalog.value("favorites").toArray()){
        const bool same=entry.toObject().value("value").toString()==value;
        listed=listed||same;
        if(!favorite&&same)continue;
        favorites.append(entry);
    }
    if(favorite&&!listed){
        const auto models=catalog.value("models").toArray();
        const bool available=std::any_of(models.cbegin(),models.cend(),[&](const QJsonValue &model){
            const auto entry=model.toObject();return entry.value("value").toString(entry.value("id").toString())==value;});
        favorites.append(QJsonObject{{"value",value},{"available",available}});
    }
    catalog.insert("favorites",favorites);
}
// Folders of the selected computer: drive and UNC paths on the local Windows
// machine (D16), POSIX paths elsewhere.
MachinePlatform::Style folderStyle(const FleetState &fleet,const QString &host) {
    return MachinePlatform::of(fleet.local().metrics,host.isEmpty());
}
// Local Windows folders do not enter the POSIX Swarm catalog. Their saved
// membership is still evidence when the selected computer confirms that exact
// canonical path. Never turn a lexical prefix or an unverified alias into it.
QString exactSavedProject(const SessionOrganization &projects,const FleetState &fleet,
    const QString &host,const QString &canonical)
{
    if(projects.groups().size()>512)return {};
    const auto style=folderStyle(fleet,host);
    QString found;int folders=0;
    for(const auto &project:projects.groups()){
        for(const auto &folder:project.folders){
            if(++folders>512)return {};
            if(project.id=="ungrouped"||project.id.isEmpty()
                ||(folder.machine==fleet.local().host?QString():folder.machine)!=host
                ||!MachinePlatform::absolute(folder.path,style)
                ||!MachinePlatform::same(folder.path,MachinePlatform::clean(folder.path,style),style)
                ||!MachinePlatform::same(folder.path,canonical,style))continue;
            // Even an inaccessible second project makes the placement ambiguous.
            if(!found.isEmpty()&&found!=project.id)return {};
            found=project.id;
        }
    }
    return found;
}
}
NewSessionDialog::NewSessionDialog(const QString &hgsPath,const FleetState &fleet,const QString &selectedHost,
    const QString &agent,const QString &project,QWidget *parent,AccountUsageStore *usage,SessionOrganization *projects)
    : QDialog(parent),m_client(hgsPath,this),m_fleet(fleet),m_initialProject(project),m_initialHost(selectedHost)
{
    m_liveProjects=projects;
    m_usageStore=usage?usage:new AccountUsageStore(hgsPath,this);m_usageStore->setFleet(fleet);
    setObjectName("newSessionDialog");setWindowTitle(tr("New session"));resize(660,730);setMinimumWidth(520);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(28,24,28,24);layout->setSpacing(18);
    auto *heading=plainLabel(tr("New session"));heading->setObjectName("heading");layout->addWidget(heading);
    auto *form=new QFormLayout;m_form=form;form->setSpacing(12);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_project=new QComboBox;m_project->setObjectName("launchProject");m_project->setAccessibleName(tr("Project"));
    m_project->setMinimumContentsLength(10);m_project->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    const auto theme=QSettings().value("workspace/theme","system").toString();
    const bool dark=theme=="dark"||(theme!="light"&&palette().color(QPalette::Window).lightness()<128);
    const QColor iconColor(dark?"#a1adbb":"#647386");
    m_generatedBranch="work-"+QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_manage=new QToolButton;m_manage->setObjectName("manageLaunchProject");
    m_manage->setAutoRaise(true);m_manage->setIcon(workspaceIcon("settings",iconColor));m_manage->setIconSize(QSize(18,18));m_manage->setFixedSize(34,34);
    m_manage->setToolTip(tr("Manage this project's folders"));m_manage->setAccessibleName(m_manage->toolTip());
    auto *projectRow=new QHBoxLayout;projectRow->setSpacing(8);projectRow->addWidget(m_project,2);projectRow->addWidget(m_manage);
    m_machine=new QComboBox;m_machine->setObjectName("launchComputer");m_machine->setAccessibleName(tr("Computer"));m_machine->setMinimumContentsLength(6);m_machine->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);projectRow->addWidget(m_machine,1);
    form->addRow(tr("Target"),projectRow);
    m_newProjectName=new QLineEdit;m_newProjectName->setObjectName("launchNewProjectName");m_newProjectName->setMaxLength(80);m_newProjectName->setPlaceholderText(tr("Project name"));
    m_newProjectField=new QStackedWidget;m_newProjectField->addWidget(m_newProjectName);m_newProjectField->addWidget(new QWidget);m_newProjectField->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    m_newProjectLabel=new QStackedWidget;m_newProjectLabel->addWidget(new QLabel(tr("New project")));m_newProjectLabel->addWidget(new QWidget);m_newProjectLabel->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);form->addRow(m_newProjectLabel,m_newProjectField);
    auto *sources=new QButtonGroup(this);auto *sourceRow=new QHBoxLayout;sourceRow->setSpacing(8);
    m_folderSource=new QPushButton(workspaceIcon("folder",iconColor),tr("Working folder"));m_folderSource->setObjectName("launchFolderSource");
    m_gitSource=new QPushButton(workspaceIcon("git-branch",iconColor),tr("Git repository"));m_gitSource->setObjectName("launchRepositorySource");
    for(auto *button:{m_folderSource,m_gitSource}){button->setCheckable(true);button->setAutoDefault(false);sources->addButton(button);sourceRow->addWidget(button);button->setStyleSheet(QString("QPushButton:checked { border-color:%1; color:%1; }").arg(dark?"#8bdfc0":"#167357"));}
    m_folderSource->setChecked(true);sourceRow->addStretch();form->addRow(tr("Source"),sourceRow);
    m_folder=new QComboBox;m_folder->setObjectName("launchProjectFolder");m_folder->setAccessibleName(tr("Working folder"));
    m_folder->setMinimumContentsLength(16);m_folder->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_folder->setPlaceholderText(tr("Choose a folder…"));
    m_browse=new QPushButton(tr("Browse…"));m_browse->setObjectName("browseLaunchFolder");m_browse->setAutoDefault(false);
    auto *folderInput=new QWidget;auto *folderRow=new QHBoxLayout(folderInput);folderRow->setContentsMargins(0,0,0,0);folderRow->setSpacing(8);folderRow->addWidget(m_folder,1);folderRow->addWidget(m_browse);
    // Stacked pages reserve the larger size hint, so changing source only swaps
    // the inputs; the shared controls below keep their positions.
    m_sourceInput=new QStackedWidget;m_sourceInput->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);m_sourceInput->addWidget(folderInput);
    m_sourceLabel=plainLabel(tr("Working folder"));m_sourceLabel->setWordWrap(false);form->addRow(m_sourceLabel,m_sourceInput);
    m_sourceDetails=new QStackedWidget;m_sourceDetails->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    m_destinationLabel=plainLabel();m_destinationLabel->setWordWrap(false);form->addRow(m_destinationLabel,m_sourceDetails);
    m_folderField=new QWidget;auto *folderField=new QVBoxLayout(m_folderField);folderField->setContentsMargins(0,0,0,0);folderField->setSpacing(6);
    m_projectPath=plainLabel();m_projectPath->setObjectName("launchFolderPath");m_projectPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_projectPath->setWordWrap(false);
    m_projectPath->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    folderField->addWidget(m_projectPath);
    m_preview=plainLabel();m_preview->setObjectName("muted");folderField->addWidget(m_preview);
    // Keep the form stable as paths and repository discovery change.
    for(auto *widget:QList<QWidget *>{m_projectPath}) {
        auto policy=widget->sizePolicy();policy.setRetainSizeWhenHidden(true);widget->setSizePolicy(policy);
    }
    m_sourceDetails->addWidget(m_folderField);
    m_repositoryUrl=new QLineEdit;m_repositoryUrl->setObjectName("launchRepositoryUrl");m_repositoryUrl->setMaxLength(4096);m_repositoryUrl->setPlaceholderText("https://github.com/owner/repository.git");m_repositoryUrl->setToolTip(tr("An SSH or HTTPS repository URL, on GitHub or another Git server."));m_sourceInput->addWidget(m_repositoryUrl);
    auto *repositoryDetails=new QWidget;auto *detailsLayout=new QVBoxLayout(repositoryDetails);detailsLayout->setContentsMargins(0,0,0,0);detailsLayout->setSpacing(6);m_sourceDetails->addWidget(repositoryDetails);
    m_repositoryDestinationField=new QWidget;auto *destinationRow=new QHBoxLayout(m_repositoryDestinationField);destinationRow->setContentsMargins(0,0,0,0);destinationRow->setSpacing(8);
    m_repositoryDestination=new QLineEdit;m_repositoryDestination->setObjectName("launchRepositoryDestination");m_repositoryDestination->setMaxLength(8192);m_repositoryDestination->setToolTip(tr("The complete new checkout path. Edit its folder name or choose a parent folder with Browse."));
    m_repositoryBrowse=new QPushButton(tr("Browse…"));m_repositoryBrowse->setObjectName("browseRepositoryDestination");m_repositoryBrowse->setAutoDefault(false);destinationRow->addWidget(m_repositoryDestination,1);destinationRow->addWidget(m_repositoryBrowse);destinationRow->setAlignment(Qt::AlignTop);detailsLayout->addWidget(m_repositoryDestinationField);
    m_repositoryState=new QWidget;auto *repositoryState=new QVBoxLayout(m_repositoryState);repositoryState->setContentsMargins(0,0,0,0);repositoryState->setSpacing(6);
    m_repositoryStatus=plainLabel();m_repositoryStatus->setObjectName("launchRepositoryStatus");repositoryState->addWidget(m_repositoryStatus);
    m_repositoryProgress=new QProgressBar;m_repositoryProgress->setRange(0,0);m_repositoryProgress->setTextVisible(false);repositoryState->addWidget(m_repositoryProgress);
    auto *recoveryRow=new QHBoxLayout;m_repositoryCheck=new QPushButton(tr("Check result"));m_repositoryCheck->setObjectName("launchRepositoryCheckResult");m_repositoryAnother=new QPushButton(tr("Choose another destination…"));m_repositoryAnother->setObjectName("launchRepositoryAnotherDestination");
    for(auto *button:{m_repositoryCheck,m_repositoryAnother}){button->setAutoDefault(false);recoveryRow->addWidget(button);}recoveryRow->addStretch();repositoryState->addLayout(recoveryRow);detailsLayout->addWidget(m_repositoryState);detailsLayout->addStretch();
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
    m_account->setItemDelegate(new AccountChoice::Delegate(m_account));m_account->setMinimumHeight(180);m_account->setMaximumHeight(250);
    m_account->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);m_account->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *accountField=new QVBoxLayout;accountField->setSpacing(6);accountField->addWidget(m_account);
    auto *accountStatusRow=new QHBoxLayout;
    m_accountStatus=plainLabel();m_accountStatus->setObjectName("launchAccountStatus");accountStatusRow->addWidget(m_accountStatus,1);
    m_refreshAccounts=new QPushButton(workspaceIcon("refresh",iconColor),tr("Refresh"));m_refreshAccounts->setObjectName("refreshLaunchAccounts");m_refreshAccounts->setAutoDefault(false);
    m_refreshAccounts->setAccessibleName(tr("Refresh accounts"));accountStatusRow->addWidget(m_refreshAccounts);
    accountField->addLayout(accountStatusRow);form->addRow(tr("Account and agent"),accountField);
    // The hidden combo keeps the selected model and its variants; the button opens
    // OpenCode's grouped picker for the selected computer and folder.
    m_model=new QComboBox;m_model->setObjectName("launchModel");m_model->setAccessibleName(tr("Model"));m_model->setMinimumContentsLength(18);m_model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelButton=new QPushButton(tr("Native default model"));m_modelButton->setObjectName("launchModelButton");m_modelButton->setAccessibleName(tr("Model"));m_modelButton->setAutoDefault(false);
    m_modelButton->setToolTip(tr("Models OpenCode offers on this computer. Native default uses OpenCode's configuration."));
    auto *modelRow=new QHBoxLayout;modelRow->setSpacing(8);modelRow->addWidget(m_modelButton,1);modelRow->addWidget(m_model);
    m_modelLabel=plainLabel(tr("Model"));form->addRow(m_modelLabel,modelRow);
    m_model->addItem(tr("Native default model"),QString());m_model->hide();m_modelButton->hide();m_modelLabel->hide();
    m_modelPopup=new QFrame(this,Qt::Popup);m_modelPopup->setObjectName("launchModelPopup");m_modelPopup->setFrameShape(QFrame::StyledPanel);
    auto *popupLayout=new QVBoxLayout(m_modelPopup);popupLayout->setContentsMargins(10,10,10,10);popupLayout->setSpacing(8);
    m_modelPicker=new OpenCodeModelPicker;m_modelPicker->setObjectName("launchModelPicker");popupLayout->addWidget(m_modelPicker,1);
    auto *nativeModel=new QPushButton(tr("Use native default model"));nativeModel->setObjectName("launchModelDefault");nativeModel->setAutoDefault(false);
    popupLayout->addWidget(nativeModel,0,Qt::AlignLeft);
    // Inspecting never starts OpenCode: a stopped service keeps the last known
    // models and offers this explicit start.
    m_openCodeService=new QWidget;m_openCodeService->setObjectName("launchOpenCodeService");
    auto *serviceRow=new QHBoxLayout(m_openCodeService);serviceRow->setContentsMargins(0,0,0,0);serviceRow->setSpacing(8);
    m_catalogStatus=plainLabel();m_catalogStatus->setObjectName("launchCatalogStatus");serviceRow->addWidget(m_catalogStatus,1);
    m_startOpenCode=new QPushButton;m_startOpenCode->setObjectName("launchStartOpenCode");m_startOpenCode->setAutoDefault(false);
    m_startOpenCode->setToolTip(tr("Start the shared OpenCode service on this computer. Zerus never stops it."));
    serviceRow->addWidget(m_startOpenCode);form->addRow(QString(),m_openCodeService);m_openCodeService->hide();
    m_effort=new QComboBox;m_effort->setObjectName("launchEffort");m_effort->setAccessibleName(tr("Effort"));
    m_effortLabel=plainLabel(tr("Effort"));form->addRow(m_effortLabel,m_effort);
    m_effort->addItem(tr("Default"),QString());m_effort->hide();m_effortLabel->hide();
    m_effort->setToolTip(tr("Variants of the selected model. Default uses the model's own setting."));
    m_name=new QLineEdit(m_generatedBranch);m_name->setObjectName("launchName");form->addRow(tr("Session name"),m_name);
    m_nameError=plainLabel();m_nameError->setObjectName("launchNameError");m_nameError->hide();form->addRow(QString(),m_nameError);
    m_openTerminal=new QCheckBox(tr("Open terminal window"));m_openTerminal->setObjectName("launchOpenTerminal");
    m_openTerminal->setChecked(QSettings().value("workspace/launchOpenTerminal",false).toBool());form->addRow(QString(),m_openTerminal);
    layout->addLayout(form);
    m_error=plainLabel();m_error->setObjectName("launchError");layout->addWidget(m_error);
    layout->addStretch();auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);m_cancel=buttons->button(QDialogButtonBox::Cancel);m_start=buttons->addButton(tr("Start session"),QDialogButtonBox::AcceptRole);m_start->setObjectName("primary");layout->addWidget(buttons);
    int actionWidth=0;const auto initialAction=m_start->text();m_start->ensurePolished();
    for(const auto &text:{tr("Start session"),tr("Clone and start"),tr("Use checkout and start"),tr("Creating worktree…"),tr("Checking folder…"),tr("Cloning…"),tr("Checking…")}){
        m_start->setText(text);actionWidth=qMax(actionWidth,m_start->sizeHint().width());
    }
    m_start->setText(initialAction);m_start->setMinimumWidth(actionWidth);
    int sourceLabelWidth=0;m_sourceLabel->ensurePolished();
    for(const auto &text:{tr("Working folder"),tr("Repository")}){
        m_sourceLabel->setText(text);sourceLabelWidth=qMax(sourceLabelWidth,m_sourceLabel->sizeHint().width());
    }
    m_sourceLabel->setText(tr("Working folder"));m_sourceLabel->setMinimumWidth(sourceLabelWidth);
    // Without a computer to start on, it only opens Machines with this computer
    // selected; enabling local sessions keeps its own confirmation there.
    m_openMachines=buttons->addButton(tr("Open Machines"),QDialogButtonBox::ActionRole);m_openMachines->setObjectName("launchOpenMachines");m_openMachines->hide();
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);connect(m_start,&QPushButton::clicked,this,&NewSessionDialog::start);
    connect(m_manage,&QToolButton::clicked,this,[this]{const auto id=m_project->currentData().toString();reject();emit projectFoldersRequested(id);});
    connect(m_openMachines,&QPushButton::clicked,this,[this]{if(m_creation)return;reject();emit machinesRequested();});
    connect(m_project,&QComboBox::activated,this,[this]{m_projectExplicit=true;if(!m_repositorySource)m_repositoryNewProject=false;updateForm();});
    connect(m_project,&QComboBox::currentIndexChanged,this,[this]{m_prefillRequest=0;loadMachines();loadFolders();});
    connect(m_machine,&QComboBox::currentIndexChanged,this,[this]{m_prefillRequest=0;m_accountPreset=false;loadFolders();});
    connect(m_browse,&QPushButton::clicked,this,&NewSessionDialog::browseFolder);
    connect(m_folderSource,&QPushButton::clicked,this,[this]{setRepositorySource(false);});
    connect(m_gitSource,&QPushButton::clicked,this,[this]{setRepositorySource(true);});
    connect(m_repositoryUrl,&QLineEdit::textChanged,this,[this]{
        if(m_repository&&!m_repository->fieldsLocked()&&!m_customDestination){
            const QSignalBlocker block(m_repositoryDestination);
            m_repositoryDestination->setText(MachinePlatform::native(m_repository->parentFolder()+'/'+RepositoryClone::suggestedName(m_repositoryUrl->text()),m_repositoryStyle));
        }
        editRepositorySource();
    });
    connect(m_repositoryDestination,&QLineEdit::textEdited,this,[this]{m_customDestination=true;});
    connect(m_repositoryDestination,&QLineEdit::textChanged,this,&NewSessionDialog::editRepositorySource);
    connect(m_repositoryBrowse,&QPushButton::clicked,this,[this]{
        if(!m_repository||m_repository->fieldsLocked())return;
        const auto path=DirectoryDialog::chooseDirectory(m_client.executable(),host(),m_repository->parentFolder(),this);
        if(!path.isEmpty())m_repositoryDestination->setText(MachinePlatform::native(path+'/'+m_repository->folderName(),m_repositoryStyle));
    });
    connect(m_repositoryCheck,&QPushButton::clicked,this,[this]{if(m_repository)m_repository->checkResult();});
    connect(m_repositoryAnother,&QPushButton::clicked,this,[this]{
        if(!m_repository)return;m_repository->chooseAnotherDestination();m_customDestination=true;
        m_repositoryDestination->setText(MachinePlatform::native(m_repository->destination(),m_repositoryStyle));
    });
    connect(m_newProjectName,&QLineEdit::textEdited,this,[this]{m_customProjectName=true;});
    connect(m_newProjectName,&QLineEdit::textChanged,this,[this]{updateForm();});
    connect(m_folder,&QComboBox::currentIndexChanged,this,[this]{
        m_prefillRequest=0;
        m_browsedFolders.remove(host());
        loadAccounts();updateModels();updateForm();updateWorktrees();
    });
    connect(m_folder,&QComboBox::activated,this,[this]{m_prefillRequest=0;updateForm();});
    connect(m_account,&QListWidget::currentItemChanged,this,[this]{m_preferredAgent=this->agent();m_preferredAccount=account();m_accountPreset=true;updateModels();updateForm();});
    connect(m_refreshAccounts,&QPushButton::clicked,this,[this]{
        // Refresh asks again for folders whose last answer was not a ready catalog.
        for(const auto &key:QStringList(m_folderOrder))
            if(key.startsWith(host()+'\n')&&!readyCatalog(m_folderCatalogs.value(key))){m_folderOrder.removeOne(key);m_folderCatalogs.remove(key);}
        m_forceUsage=true;loadAccounts(true);
    });
    connect(m_model,&QComboBox::currentIndexChanged,this,&NewSessionDialog::updateEfforts);
    connect(m_model,&QComboBox::currentIndexChanged,this,&NewSessionDialog::updateModelButton);
    connect(m_modelButton,&QPushButton::clicked,this,&NewSessionDialog::openModelPicker);
    connect(m_modelPicker,&OpenCodeModelPicker::chosen,this,&NewSessionDialog::chooseModel);
    connect(m_modelPicker,&OpenCodeModelPicker::favoriteRequested,this,&NewSessionDialog::changeFavorite);
    connect(m_modelPicker,&OpenCodeModelPicker::catalogRequested,this,[this]{
        // All models is an explicit ask: it reads the folder again unless a ready catalog is here.
        const auto machine=host().isEmpty()?m_fleet.local().host:host();
        const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
        const auto stored=m_folderCatalogs.value(modelKey());
        if(m_folder->currentData(Qt::UserRole+2).toString().isEmpty())m_modelPicker->setCatalogFailed(tr("Choose a working folder to list all of its models."));
        else if(!box||!box->ok)m_modelPicker->setCatalogFailed(tr("%1 is offline.").arg(machine));
        else if(readyCatalog(stored))m_modelPicker->setCatalog(stored);
        else if(!m_folderRequests.key(modelKey(),0))requestFolderCatalog(true);
    });
    connect(nativeModel,&QPushButton::clicked,this,[this]{m_chosenModel={};m_modelPopup->hide();m_model->setCurrentIndex(0);});
    connect(m_startOpenCode,&QPushButton::clicked,this,&NewSessionDialog::startOpenCode);
    connect(m_usageStore,&AccountUsageStore::changed,this,[this](const QString &){refreshUsage();});
    auto *usageTimer=new QTimer(this);usageTimer->setInterval(60000);
    connect(usageTimer,&QTimer::timeout,this,[this]{refreshUsage();m_account->viewport()->update();});usageTimer->start();
    connect(m_name,&QLineEdit::textChanged,this,&NewSessionDialog::updateForm);
    connect(&m_client,&HgsClient::directoriesReady,this,[this](quint64 request,const QString &machine,const QJsonObject &data){
        if(!m_validation||request!=m_validation||machine!=host())return;
        m_validation=0;
        const auto path=data.value("path").toString();
        if(!MachinePlatform::absolute(path,folderStyle(m_fleet,host()))) {m_error->setText(tr("The computer did not confirm this folder. Try again."));updateForm();return;}
        if(m_startWithWorktree&&!automaticWorktreeAvailable()){
            m_error->setText(tr("The repository could not be confirmed. Choose a working folder again."));updateForm();return;
        }
        // Only an untouched Inbox is a fallback. The owning computer confirmed
        // this exact directory; a selected project (including Inbox) always wins.
        QString inferredId;
        if(data.contains("project_inference")){
            const auto inferred=data.value("project_inference").toObject();
            if(inferred.value("schema").toInt()==1&&!m_projectCatalogSwarm.isEmpty()
                &&inferred.value("swarm_id").toString()==m_projectCatalogSwarm)
                inferredId=inferred.value("project_id").toString();
            // A foreign or invalid hint never falls back to an unrelated local
            // project with the same ID or path. Joining remains explicit.
        }else if(!m_projectExplicit&&m_project->currentData()=="ungrouped")
            inferredId=exactSavedProject(m_projects,m_fleet,host(),path);
        const int inferredIndex=m_project->findData(inferredId);
        const bool infer=!m_projectExplicit&&m_project->currentData()=="ungrouped"
            &&!inferredId.isEmpty()&&inferredId!="ungrouped"&&inferredIndex>=0;
        if(infer){const QSignalBlocker block(m_project);m_project->setCurrentIndex(inferredIndex);}
        // Listed folders and verified worktrees already belong to the project.
        // Decide before creation: the selection then moves to the new worktree.
        const auto addFolder=!infer&&m_folder->currentData().toString().isEmpty()&&!projectWorktree(m_folder->currentData(Qt::UserRole+2).toString())?path:QString();
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
        if(m_serviceStart&&id==m_serviceStart){
            // The service answered with its catalog: read this computer's accounts again.
            m_serviceStart=0;if(machine==host())loadAccounts(true);updateOpenCodeService();return;
        }
        if(id!=m_accountRequest||machine!=host())return;
        m_accountRequest=0;m_accountCatalogLoaded=true;m_accounts=data["profiles"].toArray();m_accountStatus->setText({});
        m_regroupAccounts=true;updateAccounts();requestUsage(std::exchange(m_forceUsage,false));
    });
    connect(&m_client,&HgsClient::accountsFailed,this,[this](quint64 id,const QString &machine,const QString &error){
        if(m_serviceStart&&id==m_serviceStart){
            m_serviceStart=0;if(machine==host())m_accountStatus->setText(tr("Could not start OpenCode: %1").arg(error));
            updateOpenCodeService();return;
        }
        if(id!=m_accountRequest||machine!=host())return;
        m_accountRequest=0;m_accountCatalogLoaded=true;m_forceUsage=false;updateAccounts();m_accountStatus->setText(tr("Could not load accounts: %1").arg(error));
    });
    // A folder catalog belongs to its own computer and folder: a late answer is
    // kept for that folder and never shown in another one. Only the newest
    // request issued for a folder answers it: an older answer is dropped, also
    // when it arrives after the newest one was applied.
    const auto newest=[this](quint64 request,const QString &key){
        const bool latest=m_folderLatest.value(key)==request;
        // With nothing pending for this folder, no older answer can arrive any more.
        if(!m_folderRequests.key(key,0))m_folderLatest.remove(key);
        return latest;
    };
    connect(&m_client,&HgsClient::openCodeModelsReady,this,[this,newest](quint64 request,const QString &machine,const QString &location,const QJsonObject &catalog){
        if(!m_folderRequests.contains(request))return;
        const auto key=m_folderRequests.take(request);if(!newest(request,key)||key!=machine+'\n'+location)return;
        storeFolderCatalog(key,catalog);
        if(key==modelKey()&&this->agent()=="opencode")updateModels();
    });
    connect(&m_client,&HgsClient::openCodeModelsFailed,this,[this,newest](quint64 request,const QString &machine,const QString &location,const QString &error){
        if(!m_folderRequests.contains(request))return;
        const auto key=m_folderRequests.take(request);if(!newest(request,key)||key!=machine+'\n'+location)return;
        const auto failure=OpenCodeModels::errorText(error,machine.isEmpty()?m_fleet.local().host:machine);
        // A failed refresh keeps a ready catalog; otherwise the failure is shown again, not asked again.
        const bool kept=readyCatalog(m_folderCatalogs.value(key));
        if(!kept)storeFolderCatalog(key,QJsonObject{{"failure",failure}});
        if(key!=modelKey()||this->agent()!="opencode")return;
        if(kept)m_modelPicker->setCatalogFailed(failure);else updateModels();
    });
    connect(&m_client,&HgsClient::openCodeFavoriteReady,this,[this](quint64 request,const QString &machine,const QJsonObject &){
        if(!m_favoriteRequests.contains(request))return;
        const auto change=m_favoriteRequests.take(request);
        // Favorites belong to the computer: its other folders' catalogs are stale now.
        for(const auto &key:QStringList(m_folderOrder))
            if(key.startsWith(machine+'\n')&&key!=modelKey()){m_folderOrder.removeOne(key);m_folderCatalogs.remove(key);}
        if(machine!=host())return;
        loadAccounts(true);
        // The answer for another folder or agent leaves this picker as it is.
        if(change.key!=modelKey()||this->agent()!="opencode")return;
        // updateModels passes the stored catalog and the account row again; they carry the
        // confirmed change, or the picker's next short list would bring back the state before it.
        if(auto stored=m_folderCatalogs.value(change.key);readyCatalog(stored)){
            markCatalogFavorite(stored,change.value,change.favorite);m_folderCatalogs.insert(change.key,stored);
        }
        for(qsizetype index=0;index<m_accounts.size();++index){
            auto profile=m_accounts.at(index).toObject();
            if(profile.value("provider")!="opencode"||profile.value("id").toString()!=account()||!profile.contains("model_options"))continue;
            profile.insert("model_options",markFavorite(profile.value("model_options").toArray(),change.value,change.favorite));
            m_accounts.replace(index,profile);
        }
        m_modelPicker->setFavorite(change.value,change.favorite);requestFolderCatalog(true);
    });
    connect(&m_client,&HgsClient::openCodeFavoriteFailed,this,[this](quint64 request,const QString &machine,const QString &error){
        if(!m_favoriteRequests.contains(request))return;
        const auto change=m_favoriteRequests.take(request);
        if(machine!=host()||change.key!=modelKey()||this->agent()!="opencode")return;
        m_modelPicker->setFavoritePending(change.value,false);
        const auto name=machine.isEmpty()?m_fleet.local().host:machine;
        m_favoriteError=tr("Could not change the favorite on %1: %2").arg(name,OpenCodeModels::errorText(error,name));m_error->setText(m_favoriteError);
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
void NewSessionDialog::reject(){if(!m_creation&&(!m_repository||!m_repository->cloning())){m_cloneLaunchPending=false;if(m_repository)m_repository->cancelCheck();QDialog::reject();}}
QString NewSessionDialog::host() const {return m_machine->currentData().toString();}
void NewSessionDialog::setFleet(const FleetState &fleet)
{
    m_fleet=fleet;
    m_usageStore->setFleet(fleet);
    if((m_repository&&m_repository->busy())||m_cloneLaunchPending){updateForm();return;}
    if(folderStyle(m_fleet,host())!=m_accountStyle)updateAccounts();else refreshUsage();
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
    const bool keepNew=m_repositorySource&&m_project->currentData(NewRepositoryProjectRole).toBool()&&selected.isEmpty();
    m_projects=projects;const QSignalBlocker block(m_project);m_project->clear();
    for(const auto &p:m_projects.groups())if(p.accessible)m_project->addItem(workspaceIcon("projects",QColor(p.color)),p.name,p.id);
    if(m_repositorySource){m_project->addItem(tr("New project…"),QString());m_project->setItemData(m_project->count()-1,true,NewRepositoryProjectRole);}
    int index=keepNew?m_project->count()-1:selected.isEmpty()?-1:m_project->findData(selected);if(index<0)index=m_project->findText(m_initialProject);
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
    const auto style=folderStyle(m_fleet,host());
    const auto add=[&](const QString &id,const QString &rawPath,const QString &name){
        const auto path=MachinePlatform::clean(rawPath,style);const auto leaf=MachinePlatform::leaf(path,style);
        rows.append({id,path,name.isEmpty()?(leaf.isEmpty()?MachinePlatform::native(path,style):leaf):name});
    };
    if(project)for(const auto &folder:project->folders){
        if((folder.machine==m_fleet.local().host?QString():folder.machine)!=host())continue;
        add(folder.id,folder.path,folder.name);
    }
    const auto custom=!selectedPath.isEmpty()?selectedPath:!previous.isEmpty()?previous:m_browsedFolders.value(host());
    int index=rows.isEmpty()?-1:0;
    if(!custom.isEmpty()){
        index=-1;for(int row=0;row<rows.size()&&index<0;++row)if(MachinePlatform::same(rows[row].path,custom,style))index=row;
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
            m_folder->setItemData(at,host(),Qt::UserRole+1);m_folder->setItemData(at,row.path,Qt::UserRole+2);m_folder->setItemData(at,MachinePlatform::native(row.path,style),Qt::ToolTipRole);
        }
    }
    for(int row=0;row<m_folder->count();++row)m_folder->setItemData(row,available,Qt::UserRole+3);
    m_folder->setCurrentIndex(index);
    loadAccounts();updateModels();updateForm();updateWorktrees();
}
void NewSessionDialog::browseFolder()
{
    const auto target=host(),project=m_project->currentData().toString();
    const auto path=DirectoryDialog::chooseDirectory(m_client.executable(),target,m_folder->currentData(Qt::UserRole+2).toString(),this);
    if(path.isEmpty()||host()!=target||m_project->currentData().toString()!=project)return;
    selectPath(path);
    m_browsedFolders[target]=MachinePlatform::clean(path,folderStyle(m_fleet,target));
}

void NewSessionDialog::setRepositorySource(bool enabled)
{
    if(m_repositorySource==enabled||m_validation||m_creation||(m_repository&&m_repository->busy()))return;
    m_prefillRequest=0;
    if(m_repositorySource&&!enabled)m_repositoryNewProject=m_project->currentData(NewRepositoryProjectRole).toBool();
    m_repositorySource=enabled;
    const auto newProject=m_project->findData(true,NewRepositoryProjectRole);
    if(enabled&&newProject<0){m_project->addItem(tr("New project…"),QString());m_project->setItemData(m_project->count()-1,true,NewRepositoryProjectRole);}
    else if(!enabled&&newProject>=0){
        if(m_project->currentIndex()==newProject)m_project->setCurrentIndex(qMax(0,m_project->findData(m_projects.defaultProject())));
        m_project->removeItem(newProject);
    }
    // Folder mode needs a real project; keep the repository-only choice until
    // the user explicitly chooses a project there, even the same fallback Inbox.
    if(enabled&&m_repositoryNewProject)m_project->setCurrentIndex(m_project->findData(true,NewRepositoryProjectRole));
    syncRepositoryComputer();updateForm();
    if(enabled)m_repositoryUrl->setFocus();
}
void NewSessionDialog::syncRepositoryComputer()
{
    if(!m_repositorySource)return;
    const auto style=folderStyle(m_fleet,host());
    if(m_repository&&m_repository->host()==host()&&m_repositoryStyle==style)return;
    const auto url=m_repositoryUrl->text();const auto name=m_repository?m_repository->folderName():RepositoryClone::suggestedName(url);
    delete m_repository;m_repositoryStyle=style;m_repository=new RepositoryClone(&m_client,host(),style,this);
    if(!m_repository->fieldsLocked())m_repository->setSource(url,m_repository->parentFolder(),name);
    m_customDestination=false;
    {const QSignalBlocker urlBlock(m_repositoryUrl),pathBlock(m_repositoryDestination);m_repositoryUrl->setText(m_repository->url());m_repositoryDestination->setText(MachinePlatform::native(m_repository->destination(),style));}
    if(!m_customProjectName){const QSignalBlocker project(m_newProjectName);m_newProjectName->setText(m_repository->folderName());}
    connect(m_repository,&RepositoryClone::changed,this,[this]{
        if(m_repository->fieldsLocked()){
            const QSignalBlocker url(m_repositoryUrl),destination(m_repositoryDestination);
            m_repositoryUrl->setText(m_repository->url());m_repositoryDestination->setText(MachinePlatform::native(m_repository->destination(),m_repositoryStyle));
        }
        updateForm();
    });
    connect(m_repository,&RepositoryClone::prepared,this,&NewSessionDialog::useRepository);
}
void NewSessionDialog::editRepositorySource()
{
    if(!m_repository||m_repository->fieldsLocked())return;
    auto destination=m_repositoryDestination->text();
    if(m_repositoryStyle==MachinePlatform::Style::Windows)destination.replace('\\','/');
    const auto separator=destination.lastIndexOf('/');
    auto parent=separator<0?QString():destination.left(separator);const auto name=destination.mid(separator+1);
    if(separator==0)parent="/";
    if(m_repositoryStyle==MachinePlatform::Style::Windows&&parent.endsWith(':'))parent+='/';
    if(!m_customProjectName)m_newProjectName->setText(name);
    m_repository->setSource(m_repositoryUrl->text(),parent,name);updateForm();
}
void NewSessionDialog::useRepository(const QString &path)
{
    if(!m_repository||m_repository->host()!=host())return;
    const auto target=host();const auto style=folderStyle(m_fleet,target);auto *projects=m_liveProjects?m_liveProjects:&m_projects;
    auto project=m_project->currentData().toString();
    if(m_project->currentData(NewRepositoryProjectRole).toBool())project=projects->createGroup(m_newProjectName->text().trimmed());
    if(project.isEmpty()||!projects->group(project)){
        m_error->setText(tr("The project changed. The checkout is available at %1. Choose a project to use it.").arg(path));updateForm();return;
    }
    const auto folder=projects->addFolder(project,target.isEmpty()?m_fleet.local().host:target,path,{},style);
    emit organizationChanged();m_repositorySource=false;m_repositoryNewProject=false;
    const auto snapshot=*projects;setGroups(snapshot,project);setProjectExplicit(true);
    const auto index=m_machine->findData(target);
    if(index<0||folder.isEmpty()){
        m_error->setText(tr("Checkout saved. Select its computer and folder to start a session."));updateForm();return;
    }
    m_machine->setCurrentIndex(index);if(!selectFolder(folder))selectPath(path);
    m_cloneLaunchPending=true;updateForm();
}

void NewSessionDialog::updateForm()
{
    syncRepositoryComputer();
    const bool repository=m_repositorySource;
    const auto *machine=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    const bool online=machine&&machine->ok;
    const bool newProject=repository&&m_project->currentData(NewRepositoryProjectRole).toBool();
    m_form->setRowVisible(m_newProjectField,newProject||(!repository&&m_repositoryNewProject));
    m_newProjectField->setCurrentIndex(newProject?0:1);m_newProjectLabel->setCurrentIndex(newProject?0:1);
    m_sourceInput->setCurrentIndex(repository?1:0);m_sourceDetails->setCurrentIndex(repository?1:0);
    m_sourceLabel->setText(repository?tr("Repository"):tr("Working folder"));m_destinationLabel->setText(repository?tr("Clone to"):QString());
    m_folderSource->setChecked(!repository);m_gitSource->setChecked(repository);
    const auto path=m_folder->currentData(Qt::UserRole+2).toString();
    const auto shown=MachinePlatform::native(path,folderStyle(m_fleet,host()));
    m_projectPath->setText(shown);m_projectPath->setVisible(!path.isEmpty());
    m_projectPath->setToolTip(shown);m_projectPath->ensurePolished();m_projectPath->setFixedHeight(m_projectPath->fontMetrics().height());
    const bool worktree=m_folder->currentData().toString().isEmpty()&&projectWorktree(path);
    const bool outside=!path.isEmpty()&&m_folder->currentData().toString().isEmpty()&&!worktree;
    m_preview->setProperty("outsideProject",outside);
    m_preview->setStyleSheet(outside?QStringLiteral("color: %1;").arg(palette().color(QPalette::Window).lightness()<128?"#f0b65a":"#9b6300"):QString());
    m_preview->setText(m_prefillRequest?tr("Checking the session's main working folder…"):
        path.isEmpty()?tr("Browse to choose a folder on this computer."):
        implicitInbox()?tr("A matching project is detected automatically.\nOtherwise this session stays in Inbox."):
        outside?tr("This folder is outside the project.\nStarting here will add it to this project."):
        QString());
    m_preview->setToolTip(m_preview->text());m_preview->ensurePolished();m_preview->setFixedHeight(2*m_preview->fontMetrics().lineSpacing());
    const QString nameProblem=SessionTag::problem(m_name->text());const bool validName=nameProblem.isEmpty();
    m_nameError->setText(nameProblem);m_nameError->setVisible(!validName);
    const bool busy=m_validation||m_creation||(m_repository&&m_repository->busy());
    const bool locked=busy||m_cloneLaunchPending;
    const bool noMachine=m_machine->count()==0;
    const bool automatic=m_autoPreferred&&(repository||automaticWorktreeAvailable());
    const bool ready=repository?(online&&m_repository&&m_repository->canSubmit()&&(!newProject||!m_newProjectName->text().trimmed().isEmpty())):
        (!m_prefillRequest&&!path.isEmpty()&&m_folder->currentData(Qt::UserRole+3).toBool()&&(!m_autoPreferred||!m_catalogRequest)&&!(automatic&&m_creationFailed));
    m_start->setEnabled(!noMachine&&m_account->currentItem()&&(!m_accountRequest||agent()=="sh")&&!busy&&validName&&ready);
    m_start->setText(m_creation?tr("Creating worktree…"):m_validation?tr("Checking folder…"):
        repository?(m_repository&&m_repository->busy()?(m_repository->cloning()?tr("Cloning…"):tr("Checking…")):
            (m_repository&&!m_repository->createdPath().isEmpty()?tr("Use checkout and start"):tr("Clone and start"))):tr("Start session"));
    for(auto *widget:QList<QWidget *>{m_project,m_machine,m_folder,m_account,m_model,m_effort,m_name,m_manage,m_worktreeOptions})widget->setEnabled(!locked);
    m_refreshAccounts->setEnabled(!busy&&!noMachine&&!m_accountRequest);
    m_folderSource->setEnabled(!locked);m_gitSource->setEnabled(!locked);m_newProjectName->setEnabled(!locked);
    m_autoWorktree->setEnabled(!busy&&(repository||automaticWorktreeAvailable()));m_plainFolder->setEnabled(!busy);m_cancel->setEnabled(!m_creation&&(!m_repository||!m_repository->cloning()));
    m_autoWorktree->setChecked(automatic);m_plainFolder->setChecked(!automatic);
    const auto automaticPath=repository&&m_repository?m_repository->destination()+'-'+m_generatedBranch:automaticWorktreePath();
    m_worktreeTarget->setText(automatic?MachinePlatform::native(automaticPath,folderStyle(m_fleet,host())):QString());m_worktreeTarget->setToolTip(automatic?tr("New branch: %1\nStart from: HEAD").arg(m_generatedBranch):QString());
    m_worktreeTarget->ensurePolished();m_worktreeTarget->setFixedHeight(2*m_worktreeTarget->fontMetrics().lineSpacing());
    m_browse->setEnabled(!locked&&!noMachine&&machine&&machine->ok);
    const bool fieldsLocked=!m_repository||m_repository->fieldsLocked();
    for(auto *widget:QList<QWidget *>{m_repositoryUrl,m_repositoryDestination,m_repositoryBrowse})widget->setEnabled(!locked&&!fieldsLocked&&online);
    m_repositoryStatus->setText(m_repository?m_repository->statusText():QString());m_repositoryStatus->setVisible(!m_repositoryStatus->text().isEmpty());
    m_repositoryProgress->setVisible(m_repository&&m_repository->busy());
    m_repositoryCheck->setVisible(m_repository&&m_repository->uncertain());m_repositoryAnother->setVisible(m_repository&&m_repository->uncertain());
    m_repositoryCheck->setEnabled(!busy&&online);m_repositoryAnother->setEnabled(!busy);
    m_repositoryState->setVisible(m_repository&&(m_repository->busy()||!m_repositoryStatus->text().isEmpty()||m_repository->uncertain()));
    m_worktrees->setEnabled(!busy&&!m_catalogRequest&&!m_worktreeCatalog["stale"].toBool()&&!m_worktreeCatalog["worktrees"].toArray().isEmpty());
    m_newWorktree->setEnabled(!busy&&!m_catalogRequest&&machine&&machine->ok&&automaticWorktreeAvailable());
    m_worktreeOptions->setEnabled(!repository&&(m_worktrees->isEnabled()||m_newWorktree->isEnabled()));
    m_openTerminal->setEnabled(!busy&&agent()!="dsh"&&agent()!="opencode");
    m_openTerminal->setToolTip(agent()=="dsh"?tr("DeepSeek opens in the workspace's Native UI."):agent()=="opencode"?tr("OpenCode runs through its native server and opens in Activity."):tr("Also open an external terminal for this session."));
    const auto offline=tr("This computer is unavailable. Choose another folder or reconnect it.");
    if(!repository&&!path.isEmpty()&&!m_folder->currentData(Qt::UserRole+3).toBool())m_error->setText(offline);
    else if(m_error->text()==offline)m_error->clear();
    const auto noAccount=tr("Restore or add an account in Agent accounts to start this agent.");
    if(m_account->count()==0)m_error->setText(noAccount);else if(m_error->text()==noAccount)m_error->clear();
    const auto loadingAccount=tr("Loading this session's account…"),missingAccount=tr("This session's account is unavailable. Choose another account.");
    if(m_accountPreset&&!m_account->currentItem())m_error->setText(m_accountRequest?loadingAccount:missingAccount);
    else if(m_error->text()==loadingAccount||m_error->text()==missingAccount)m_error->clear();
    const auto remoteOnly=tr("Add a remote machine in Machines to start sessions from this computer.");
    const auto enableOrRemote=tr("Enable local sessions for this computer in Machines, or add a remote machine.");
    if(noMachine)m_error->setText(LocalSessionHost::canEnable(m_fleet.local())?enableOrRemote:remoteOnly);
    else if(m_error->text()==remoteOnly||m_error->text()==enableOrRemote)m_error->clear();
    m_openMachines->setVisible(noMachine);
    updateModelButton();updateOpenCodeService();
    if(m_cloneLaunchPending&&m_start->isEnabled())QTimer::singleShot(0,this,[this]{
        if(m_cloneLaunchPending&&m_start->isEnabled()){
            m_cloneLaunchPending=false;
            if(m_liveProjects&&!m_liveProjects->group(m_project->currentData().toString())){
                m_error->setText(tr("Project changed. Select the checkout again before starting."));updateForm();return;
            }
            start();
        }
    });
}
void NewSessionDialog::start()
{
    if(!m_start->isEnabled())return;
    // Spec D5: ask before this computer's session host of another Windows session
    // starts the agent. Cancel keeps the form; no folder check or worktree starts.
    if(!LocalSessionHost::confirmLaunch(this,m_fleet.local(),host())||!m_start->isEnabled())return;
    if(m_repositorySource){if(m_repository)m_repository->begin();return;}
    m_startWithWorktree=m_autoPreferred&&automaticWorktreeAvailable();
    m_error->setText(tr("Checking folder…"));m_validation=m_client.requestDirectories(host(),m_folder->currentData(Qt::UserRole+2).toString());updateForm();
}
bool NewSessionDialog::automaticWorktreeAvailable() const
{
    const auto path=m_folder->currentData(Qt::UserRole+2).toString();
    const auto root=m_worktreeCatalog["selected_root"].toString();
    if(m_catalogRequest||m_worktreeCatalog["state"]!="ok"||m_worktreeCatalog["stale"].toBool()
        ||m_worktreeCatalog["common_dir"].toString().isEmpty()||!QDir::isAbsolutePath(root)||QFileInfo(root).fileName().isEmpty())return false;
    if(m_worktreeCatalog["partial"].toBool()){
        // Git confirms the requested canonical cwd and selected root separately
        // from the bounded list. The selected checkout may be beyond its cap.
        const auto style=folderStyle(m_fleet,host());const auto selected=m_worktreeCatalog["path"].toString();
        if(!MachinePlatform::absolute(selected,style)||!MachinePlatform::within(selected,root,style))return false;
        // HgsClient also derives sibling snapshots from listed entries. Those
        // cannot turn a known missing or bare row into an eligible checkout.
        for(const auto &value:m_worktreeCatalog["worktrees"].toArray()){
            const auto entry=value.toObject();
            if(MachinePlatform::same(root,entry["path"].toString(),style))return entry["available"].toBool()&&entry["kind"]!="bare";
        }
        return true;
    }
    for(const auto &value:m_worktreeCatalog["worktrees"].toArray()){
        const auto entry=value.toObject();
        if(entry["available"].toBool()&&entry["kind"]!="bare"&&MachinePlatform::within(path,entry["path"].toString(),folderStyle(m_fleet,host())))return true;
    }
    return false;
}
QString NewSessionDialog::automaticWorktreePath() const
{
    const QFileInfo root(m_worktreeCatalog["selected_root"].toString());
    return QDir(root.path()).filePath(root.fileName()+'-'+m_generatedBranch);
}
bool NewSessionDialog::implicitInbox() const
{
    return !m_projectExplicit && m_project->currentData()=="ungrouped";
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
    const auto model=agent()=="opencode"?m_model->currentData().toString():QString();
    emit launchRequested(host(),agent(),target,m_name->text(),account(),m_project->currentData().toString(),m_openTerminal->isChecked()&&agent()!="dsh"&&agent()!="opencode",addFolder,
        model,model.isEmpty()?QString():m_effort->currentData().toString());accept();
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
        m_accounts={};m_accountPreset=false;m_accountCatalogLoaded=false;m_regroupAccounts=true;
    }
    m_accountHost=host();m_accountStatus->setText(tr("Loading accounts…"));
    m_accountRequest=m_client.requestAccounts(host(),{"ls"});updateAccounts();
}
void NewSessionDialog::updateAccounts()
{
    if(m_machine->count()==0){const QSignalBlocker block(m_account);m_account->clear();updateModels();updateForm();return;}
    const auto style=folderStyle(m_fleet,host());
    // Use the Accounts page's identity/scope rules, applied only to this
    // computer's catalog. An account can have several local launch bindings.
    // Group when the catalog arrives: later limits only refresh these rows, even
    // when they show two rows to be one account (the next dialog joins them).
    if(m_regroupAccounts||style!=m_accountStyle){
        m_regroupAccounts=false;m_accountStyle=style;
        QJsonArray localProfiles;
        // The selected computer's agents only: the local Windows computer hides some (spec D16).
        QStringList providers;for(const auto &provider:m_agents)if(AccountCatalog::launchable(provider,style))providers.append(provider);
        for(const auto &value:m_accounts){const auto provider=value.toObject()["provider"].toString();if(!provider.isEmpty()&&provider!="sh"&&!providers.contains(provider)&&AccountCatalog::launchable(provider,style))providers.append(provider);}
        for(const auto &provider:providers){
            for(const auto &value:m_accounts){
                auto profile=value.toObject();if(profile["provider"]!=provider||profile["id"].toString().isEmpty())continue;
                profile["host"]=host();profile["usage"]=cachedUsage(profile);localProfiles.append(profile);
            }
        }
        m_accountGroups=AccountCatalog::group(localProfiles);
    }
    const auto remembered=QJsonDocument::fromJson(QSettings().value("workspace/launchAccounts").toByteArray()).object().value(host()).toObject();
    const auto current=m_account->currentItem()?m_account->currentItem()->data(ProfileRole).toJsonObject():QJsonObject();
    const auto currentId=current["host"].toString()==host()&&current["provider"]==m_preferredAgent?current["id"].toString():QString();
    QList<QJsonObject> profiles;QList<QJsonArray> members;
    for(const auto &value:m_accountGroups){
        const auto group=value.toObject();QJsonObject chosen;int best=-1;
        for(const auto &member:group["members"].toArray()){
            const auto profile=member.toObject();const auto provider=profile["provider"].toString(),id=profile["id"].toString();
            const int rank=m_accountPreset&&provider==m_preferredAgent&&id==m_preferredAccount?5:
                provider==m_preferredAgent&&id==currentId?4:id==remembered[provider].toString()?3:
                profile["is_default"].toBool()?2:profile["native"].toBool()?1:0;
            if(rank>best){chosen=profile;best=rank;}
        }
        members.append(group["members"].toArray());chosen["usage"]=accountUsage(members.last());profiles.append(chosen);
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
        item->setData(Qt::UserRole,id);item->setData(ProviderRole,provider);item->setData(ProfileRole,profile);item->setData(MembersRole,members[row]);
        item->setIcon(workspaceIcon(m_agents.contains(provider)?provider:QString("terminal"),IdentityBadges::providerColor(provider,dark)));
        if(provider==m_preferredAgent){
            if(defaultIndex<0||profile["is_default"].toBool())defaultIndex=row;
            if(id==lastAccount)lastIndex=row;
            if(id==m_preferredAccount)explicitIndex=row;
            if(id==currentId)currentIndex=row;
        }
    }
    auto *shell=same?m_account->item(profiles.size()):new QListWidgetItem(m_account);
    shell->setText(style==MachinePlatform::Style::Windows?tr("Run PowerShell"):tr("Run shell"));shell->setData(Qt::UserRole,QString());shell->setData(ProviderRole,"sh");
    shell->setData(UsageTextRole,tr("Interactive terminal session"));shell->setIcon(workspaceIcon("terminal",iconColor));
    shell->setData(Qt::AccessibleTextRole,shell->text());
    if(m_preferredAgent=="sh")defaultIndex=lastIndex=explicitIndex=profiles.size();
    // Explicit selections never silently switch accounts; remembered ones may
    // fall back to the provider's default when the old profile no longer exists.
    const int selected=m_accountPreset?explicitIndex:currentIndex>=0?currentIndex:lastIndex>=0?lastIndex:defaultIndex>=0?defaultIndex:0;
    // A temporary shell-only list must not replace the intended agent while
    // its computer's authoritative account catalog is still loading.
    const int target=m_accountRequest&&profiles.isEmpty()&&m_preferredAgent!="sh"?-1:selected;
    // Clearing through the view also marks the empty choice as set, so focus does
    // not pick the first row: on macOS this list takes the dialog's first focus,
    // and while loading that row is Run shell.
    if(target<0)m_account->setCurrentIndex({});else m_account->setCurrentRow(target);
    updateUsage();updateModels();updateForm();
}

// Opening and Refresh ask for current limits of this computer's accounts. The
// shared store skips those it checked within its interval unless Refresh forces it.
void NewSessionDialog::requestUsage(bool force)
{
    for(int row=0;row<m_account->count();++row)
        for(const auto &value:m_account->item(row)->data(MembersRole).toJsonArray()){
            const auto profile=value.toObject();
            if(profile["installed"].toBool())m_usageStore->ensure(AccountUsageRef::profile(profile),force);
        }
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

QJsonObject NewSessionDialog::accountUsage(const QJsonArray &members) const
{
    QJsonObject best;
    for(const auto &member:members){
        const auto usage=cachedUsage(member.toObject());
        if(best.isEmpty()||AccountCatalog::betterUsage(usage,best))best=usage;
    }
    return best;
}

// New limits change rows in place: the list keeps its rows, choice and scroll.
void NewSessionDialog::refreshUsage()
{
    for(int row=0;row<m_account->count();++row){
        auto *item=m_account->item(row);auto profile=item->data(ProfileRole).toJsonObject();
        if(profile.isEmpty())continue;
        const auto usage=accountUsage(item->data(MembersRole).toJsonArray());
        if(profile["usage"]!=usage){profile["usage"]=usage;item->setData(ProfileRole,profile);}
    }
    updateUsage();
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
    const auto style=folderStyle(m_fleet,host());
    if(!MachinePlatform::absolute(path,style))return;
    if(!automaticWorktree)m_autoPreferred=false;
    m_browsedFolders.remove(host());
    loadFolders(MachinePlatform::clean(path,style));
}
QString NewSessionDialog::matchingProjectFolder(const QString &path,const QString &canonicalPath) const
{
    QString best;const auto style=folderStyle(m_fleet,host());
    if(const auto *project=m_projects.group(m_project->currentData().toString()))
        for(const auto &folder:project->folders){
            if((folder.machine==m_fleet.local().host?QString():folder.machine)!=host())continue;
            const auto root=MachinePlatform::clean(folder.path,style);
            if((MachinePlatform::within(path,root,style)||MachinePlatform::within(canonicalPath,root,style))&&root.size()>best.size())best=root;
        }
    return best.isEmpty()?path:best;
}
void NewSessionDialog::prefillSessionFolder(const QString &path,const QString &canonicalPath)
{
    const auto style=folderStyle(m_fleet,host());
    if(!MachinePlatform::absolute(path,style))return;
    m_prefillPath=MachinePlatform::clean(path,style);
    m_prefillCanonicalPath=MachinePlatform::absolute(canonicalPath,style)?MachinePlatform::clean(canonicalPath,style):QString();
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
    const auto style=folderStyle(m_fleet,host());
    const auto selected=MachinePlatform::clean(catalog["selected_root"].toString(),style);
    if(!MachinePlatform::within(m_prefillPath,selected,style)&&!MachinePlatform::within(m_prefillCanonicalPath,selected,style))return;
    bool linked=false;QString main;
    for(const auto &value:catalog["worktrees"].toArray()){
        const auto entry=value.toObject();const auto root=MachinePlatform::clean(entry["path"].toString(),style);
        if(!MachinePlatform::absolute(root,style)||!entry["available"].toBool())continue;
        if(MachinePlatform::same(root,selected,style)&&entry["kind"]=="linked")linked=true;
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
    const auto style=folderStyle(m_fleet,host());
    const auto contains=[&](const QString &folder){
        for(const auto &value:catalog["worktrees"].toArray()){
            const auto entry=value.toObject();const auto root=MachinePlatform::clean(entry["path"].toString(),style);
            if(entry["kind"]=="bare"||!entry["available"].toBool())continue;
            if(MachinePlatform::within(MachinePlatform::clean(folder,style),root,style))return true;
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

// The short list is the account row's, or the folder's catalog once it is ready.
// Every change of computer, folder or agent first resets the picker (its
// contract), then sets its list, catalog and current model in the same turn.
void NewSessionDialog::updateModels()
{
    const bool enabled=agent()=="opencode";
    const auto row=openCodeProfile();
    const auto key=modelKey();
    const auto context=enabled?key:QString();
    const bool changed=context!=m_modelContext;
    if(changed){
        m_modelContext=context;m_modelPopup->hide();m_modelPicker->setCatalog({});m_shownCatalog={};
        // A choice belongs to its computer; another folder keeps it only when its ready catalog lists it.
        const auto chosen=m_chosenModel.value("value").toString();const auto stored=m_folderCatalogs.value(key);
        const auto listed=[&]{
            for(const auto &list:{stored["models"].toArray(),stored["model_options"].toArray()})
                for(const auto &entry:list){
                    const auto option=entry.toObject();
                    if(option["value"].toString(option["id"].toString())==chosen)return true;
                }
            return false;
        };
        if(m_chosenHost!=host())m_chosenModel={};
        else if(enabled&&!chosen.isEmpty()&&m_chosenKey!=key){if(readyCatalog(stored)&&listed())m_chosenKey=key;else m_chosenModel={};}
    }
    // A new context starts from the choice it accepted above, else the native
    // default; within one context the selected model stays, listed or not (an
    // unstarred favorite included).
    const auto selected=changed?m_chosenModel.value("value").toString():m_model->currentData().toString();
    const QJsonObject kept{{"value",selected},{"label",m_model->currentData(LabelRole).toString()},
        {"effort_options",QJsonArray::fromStringList(m_model->currentData(EffortRole).toStringList())},
        {"effort_labels",QJsonArray::fromStringList(m_model->currentData(EffortLabelRole).toStringList())}};
    const auto catalog=m_folderCatalogs.value(key);const bool ready=enabled&&readyCatalog(catalog);
    const QSignalBlocker blocker(m_model);
    m_model->clear();m_model->addItem(tr("Native default model"),QString());
    if(enabled){
        addModelOptions(m_model,row["model_options"].toArray());
        if(ready)addModelOptions(m_model,catalog["model_options"].toArray());
        if(!m_chosenModel.isEmpty())addModelOptions(m_model,QJsonArray{m_chosenModel});
        if(!changed&&!selected.isEmpty())addModelOptions(m_model,QJsonArray{kept});
        const auto &shortList=ready?catalog:row;
        m_modelPicker->setShortList(shortList["model_options"].toArray(),shortList["model_options_truncated"].toBool());
        // Another answer (a stopped service, a failure) shows its reason over the row's list.
        if(catalog!=m_shownCatalog){
            m_shownCatalog=catalog;
            if(catalog.contains("failure"))m_modelPicker->setCatalogFailed(catalog["failure"].toString());
            else if(!catalog.isEmpty())m_modelPicker->setCatalog(catalog);
        }
        const auto machine=host().isEmpty()?m_fleet.local().host:host();
        const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
        const bool supported=OpenCodeModels::catalogSupported(row),online=box&&box->ok;
        // Fleet polls reach here: stars are redrawn only when this changes.
        const auto availability=QString::number(supported)+QString::number(online)+machine;
        if(changed||availability!=m_pickerAvailability){
            m_pickerAvailability=availability;
            m_modelPicker->setAllModelsAvailable(supported,OpenCodeModels::updateText(machine));
            m_modelPicker->setFavoritesAvailable(supported&&online,supported?tr("%1 is offline.").arg(machine):OpenCodeModels::updateText(machine));
        }
    }
    m_model->setCurrentIndex(qMax(0,m_model->findData(selected)));
    m_modelButton->setVisible(enabled);m_modelLabel->setVisible(enabled);
    updateEfforts();updateModelButton();updateOpenCodeService();
}
// A changed model keeps the chosen effort only when it offers that variant too.
void NewSessionDialog::updateEfforts()
{
    const auto selected=m_effort->currentData().toString();
    const auto efforts=m_model->currentData(EffortRole).toStringList(),labels=m_model->currentData(EffortLabelRole).toStringList();
    const bool enabled=agent()=="opencode"&&!m_model->currentData().toString().isEmpty()&&!efforts.isEmpty();
    const QSignalBlocker blocker(m_effort);
    m_effort->clear();m_effort->addItem(tr("Default"),QString());
    if(enabled)for(int index=0;index<efforts.size();++index)m_effort->addItem(labels.value(index,efforts[index]),efforts[index]);
    m_effort->setCurrentIndex(qMax(0,m_effort->findData(selected)));
    m_effort->setVisible(enabled);m_effortLabel->setVisible(enabled);
}
QJsonObject NewSessionDialog::openCodeProfile() const
{
    if(agent()!="opencode")return {};
    for(const auto &value:m_accounts){
        const auto profile=value.toObject();
        if(profile["provider"]=="opencode"&&profile["id"].toString()==account())return profile;
    }
    return {};
}
void NewSessionDialog::updateOpenCodeService()
{
    const auto profile=openCodeProfile();const auto state=profile["catalog_state"].toString();
    const bool starting=m_serviceStart&&m_serviceHost==host();
    QString reason;
    if(!profile.isEmpty()&&!profile["installed"].toBool(true))reason=profile["install_reason"].toString();
    else if(!profile.isEmpty()&&!state.isEmpty()&&state!="ready")reason=profile["catalog_reason"].toString();
    const bool offer=starting||(state=="not_running"&&profile["installed"].toBool(true));
    const auto machine=host().isEmpty()?m_fleet.local().host:host();
    // As in Accounts: an offline computer cannot be asked to start it.
    const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    const bool online=box&&box->ok;
    m_catalogStatus->setText(reason);m_catalogStatus->setVisible(!reason.isEmpty());
    m_startOpenCode->setText(starting?tr("Starting OpenCode…"):tr("Start OpenCode on %1").arg(machine));
    m_startOpenCode->setVisible(offer);m_startOpenCode->setEnabled(online&&!starting&&!m_validation&&!m_creation&&!m_accountRequest);
    m_startOpenCode->setToolTip(online?tr("Start the shared OpenCode service on this computer. Zerus never stops it."):tr("%1 is offline.").arg(machine));
    m_openCodeService->setVisible(offer||!reason.isEmpty());
}
// Explicit only: hgs starts the shared service when none is registered and alive.
void NewSessionDialog::startOpenCode()
{
    if(m_serviceStart||!m_startOpenCode->isEnabled()||openCodeProfile()["catalog_state"]!="not_running")return;
    m_serviceHost=host();m_accountStatus->clear();
    m_serviceStart=m_client.requestAccounts(m_serviceHost,{"start","native-opencode","--json"});
    updateOpenCodeService();
}
QString NewSessionDialog::modelKey() const {return host()+'\n'+m_folder->currentData(Qt::UserRole+2).toString();}
// The button names the selected model. The picker opens only with this
// computer's OpenCode row: until then its context is not complete.
void NewSessionDialog::updateModelButton()
{
    const auto label=m_model->currentData(LabelRole).toString();
    m_modelButton->setText(m_model->currentIndex()<=0?tr("Native default model"):label.isEmpty()?m_model->currentData().toString():label);
    const bool ready=agent()=="opencode"&&!openCodeProfile().isEmpty()&&!m_validation&&!m_creation;
    m_modelButton->setEnabled(ready);if(!ready)m_modelPopup->hide();
    m_modelPicker->setCurrent(m_model->currentData().toString());
}
void NewSessionDialog::openModelPicker()
{
    if(!m_modelButton->isEnabled())return;
    m_modelPopup->resize(qMax(440,m_modelButton->width()),420);
    m_modelPopup->move(m_modelButton->mapToGlobal(QPoint(0,m_modelButton->height()+4)));
    m_modelPopup->show();m_modelPicker->setFocus(Qt::PopupFocusReason);
    // Opening asks once per computer and folder; a stored answer is shown again.
    requestFolderCatalog(false);
}
// `account models` for the selected folder, only from an hgs that has it on an
// online computer. Nothing here starts OpenCode or asks again on its own.
void NewSessionDialog::requestFolderCatalog(bool refresh)
{
    const auto folder=m_folder->currentData(Qt::UserRole+2).toString(),key=modelKey();
    const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    if(agent()!="opencode"||folder.isEmpty()||!box||!box->ok||!OpenCodeModels::catalogSupported(openCodeProfile()))return;
    if(!refresh&&(m_folderCatalogs.contains(key)||m_folderRequests.key(key,0)))return;
    const auto request=m_client.requestOpenCodeModels(host(),folder);
    m_folderRequests.insert(request,key);m_folderLatest.insert(key,request);
}
// Answers of at most four folders: a catalog can be large. The current one stays.
void NewSessionDialog::storeFolderCatalog(const QString &key,const QJsonObject &answer)
{
    m_folderOrder.removeOne(key);m_folderOrder.append(key);m_folderCatalogs.insert(key,answer);
    while(m_folderOrder.size()>4){
        const auto oldest=m_folderOrder.first()==modelKey()?m_folderOrder.at(1):m_folderOrder.first();
        m_folderOrder.removeOne(oldest);m_folderCatalogs.remove(oldest);
    }
}
// The picker's choice is the only way its model becomes the selection.
void NewSessionDialog::chooseModel(const QString &value)
{
    const auto model=m_modelPicker->model(value);if(!model||agent()!="opencode")return;
    m_chosenModel=QJsonObject{{"value",model->value},{"label",model->label},
        {"effort_options",QJsonArray::fromStringList(model->efforts)},{"effort_labels",QJsonArray::fromStringList(model->effortLabels)}};
    m_chosenHost=host();m_chosenKey=modelKey();
    addModelOptions(m_model,QJsonArray{m_chosenModel});
    m_modelPopup->hide();m_model->setCurrentIndex(qMax(0,m_model->findData(value)));
}
// One request identity per star click (HgsClient makes it). A failure is shown, never retried.
void NewSessionDialog::changeFavorite(const QString &value,bool favorite)
{
    const auto *box=host().isEmpty()?&m_fleet.local():m_fleet.peer(host());
    if(agent()!="opencode"||!box||!box->ok||!OpenCodeModels::catalogSupported(openCodeProfile()))return;
    if(!m_favoriteError.isEmpty()&&m_error->text()==m_favoriteError)m_error->clear();
    m_favoriteError.clear();m_modelPicker->setFavoritePending(value,true);
    m_favoriteRequests.insert(m_client.requestOpenCodeFavorite(host(),value,favorite),{modelKey(),value,favorite});
}
