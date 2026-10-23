#include "CloneRepositoryDialog.h"
#include "DirectoryDialog.h"
#include "HgsClient.h"
#include "RepositoryClone.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

QString CloneRepositoryDialog::suggestedName(const QString &url){return RepositoryClone::suggestedName(url);}
QString CloneRepositoryDialog::createdPath() const {return m_operation->createdPath();}
CloneRepositoryDialog::CloneRepositoryDialog(HgsClient *client,const QString &host,const QString &machine,
    MachinePlatform::Style style,const SessionOrganization &projects,const QString &project,bool startSession,QWidget *parent)
    :QDialog(parent),m_client(client),m_operation(new RepositoryClone(client,host,style,this)),m_style(style),m_startSession(startSession)
{
    setObjectName("cloneRepositoryDialog");setWindowTitle(tr("Clone repository"));setMinimumWidth(520);resize(640,530);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(24,22,24,22);layout->setSpacing(14);
    auto label=[&](const QString &text){auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);return l;};
    auto *heading=label(tr("Clone repository"));heading->setObjectName("heading");layout->addWidget(heading);
    auto *context=label(tr("Computer: %1").arg(machine));context->setObjectName("cloneComputer");layout->addWidget(context);
    auto *form=new QFormLayout;form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setSpacing(12);
    m_url=new QLineEdit;m_url->setObjectName("cloneRepositoryUrl");m_url->setPlaceholderText("git@example.org:team/project.git");m_url->setMaxLength(4096);form->addRow(tr("Repository"),m_url);
    m_parent=new QLineEdit(m_operation->parentFolder());m_parent->setObjectName("cloneParentFolder");
    m_browse=new QPushButton(tr("Browse…"));m_browse->setAutoDefault(false);m_browse->setObjectName("cloneBrowseParent");
    auto *parentRow=new QHBoxLayout;parentRow->addWidget(m_parent,1);parentRow->addWidget(m_browse);form->addRow(tr("Parent folder"),parentRow);
    m_name=new QLineEdit;m_name->setMaxLength(80);m_name->setObjectName("cloneFolderName");form->addRow(tr("Folder name"),m_name);
    m_path=label({});m_path->setObjectName("cloneDestination");m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);form->addRow(tr("Destination"),m_path);
    m_project=new QComboBox;m_project->setObjectName("cloneProject");
    for(const auto &p:projects.groups())if(p.accessible)m_project->addItem(p.name,p.id);
    m_project->addItem(tr("New project…"),QString());
    const auto selected=m_project->findData(project);m_project->setCurrentIndex(selected>=0?selected:m_project->count()-1);
    form->addRow(tr("Project"),m_project);
    m_projectName=new QLineEdit;m_projectName->setMaxLength(80);m_projectName->setObjectName("cloneNewProjectName");form->addRow(tr("New project name"),m_projectName);layout->addLayout(form);
    layout->addWidget(label(tr("Uses Git access configured on this computer. The checkout remains available if the session cannot start.")));
    m_error=label({});m_error->setObjectName("cloneRepositoryStatus");layout->addWidget(m_error);
    m_progress=new QProgressBar;m_progress->setRange(0,0);m_progress->setTextVisible(false);m_progress->hide();layout->addWidget(m_progress);layout->addStretch();
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);m_cancel=buttons->button(QDialogButtonBox::Cancel);
    m_accept=buttons->addButton(startSession?tr("Clone and start"):tr("Clone repository"),QDialogButtonBox::AcceptRole);m_accept->setObjectName("cloneRepositoryConfirm");
    m_check=buttons->addButton(tr("Check result"),QDialogButtonBox::ActionRole);m_check->setObjectName("cloneCheckResult");m_another=new QPushButton(tr("Choose another destination…"));m_another->setAutoDefault(false);m_another->setObjectName("cloneAnotherDestination");layout->addWidget(m_another,0,Qt::AlignLeft);
    layout->addWidget(buttons);
    connect(m_another,&QPushButton::clicked,m_operation,&RepositoryClone::chooseAnotherDestination);
    connect(buttons,&QDialogButtonBox::rejected,this,&CloneRepositoryDialog::reject);
    connect(m_accept,&QPushButton::clicked,m_operation,&RepositoryClone::begin);
    connect(m_check,&QPushButton::clicked,m_operation,&RepositoryClone::checkResult);
    connect(m_browse,&QPushButton::clicked,this,[this]{const auto path=DirectoryDialog::chooseDirectory(m_client->executable(),m_operation->host(),m_parent->text(),this);if(!path.isEmpty())m_parent->setText(path);});
    const auto source=[this]{m_operation->setSource(m_url->text(),m_parent->text(),m_name->text());};
    connect(m_url,&QLineEdit::textChanged,this,[this,source]{if(!m_customName)m_name->setText(suggestedName(m_url->text()));source();});
    connect(m_name,&QLineEdit::textEdited,this,[this]{m_customName=true;});
    connect(m_name,&QLineEdit::textChanged,this,source);
    connect(m_parent,&QLineEdit::textChanged,this,source);
    connect(m_projectName,&QLineEdit::textEdited,this,[this]{m_customProject=true;});
    connect(m_projectName,&QLineEdit::textChanged,this,[this]{updateForm();});
    connect(m_project,&QComboBox::currentIndexChanged,this,[this]{updateForm();});
    connect(m_operation,&RepositoryClone::changed,this,&CloneRepositoryDialog::updateForm);
    connect(m_operation,&RepositoryClone::prepared,this,&QDialog::accept);
    m_customName=!m_operation->url().isEmpty();updateForm();m_url->setFocus();
}
QString CloneRepositoryDialog::projectId() const {return m_project->currentData().toString();}
QString CloneRepositoryDialog::newProjectName() const {return projectId().isEmpty()?m_projectName->text().trimmed():QString();}
void CloneRepositoryDialog::reject(){if(m_operation->cloning())return;m_operation->cancelCheck();QDialog::reject();}
void CloneRepositoryDialog::updateForm()
{
    const bool busy=m_operation->busy(),locked=m_operation->fieldsLocked();
    {const QSignalBlocker url(m_url),parent(m_parent),name(m_name);m_url->setText(m_operation->url());m_parent->setText(m_operation->parentFolder());m_name->setText(m_operation->folderName());}
    if(!m_customProject){const QSignalBlocker project(m_projectName);m_projectName->setText(m_operation->folderName());}
    for(auto *w:QList<QWidget *>{m_url,m_parent,m_name,m_browse})w->setEnabled(!locked);
    m_project->setEnabled(!busy);m_projectName->setEnabled(!busy&&projectId().isEmpty());
    m_path->setText(MachinePlatform::native(m_operation->destination(),m_style));m_error->setText(m_operation->statusText());
    m_accept->setText(m_operation->createdPath().isEmpty()?(m_startSession?tr("Clone and start"):tr("Clone repository")):(m_startSession?tr("Use checkout and start"):tr("Use checkout")));
    m_accept->setEnabled(m_operation->canSubmit()&&(!projectId().isEmpty()||!newProjectName().isEmpty()));
    m_cancel->setEnabled(!m_operation->cloning());m_check->setVisible(m_operation->uncertain());m_check->setEnabled(!busy);
    m_another->setVisible(m_operation->uncertain());m_another->setEnabled(!busy);m_progress->setVisible(busy);
}
