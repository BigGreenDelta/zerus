#include "CloneRepositoryDialog.h"
#include "DirectoryDialog.h"
#include "HgsClient.h"
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
#include <QUuid>
#include <QVBoxLayout>

QString CloneRepositoryDialog::suggestedName(const QString &url)
{
    auto value=url.trimmed();while(value.endsWith('/'))value.chop(1);
    value=value.mid(qMax(value.lastIndexOf('/'),value.lastIndexOf(':'))+1);
    if(value.endsWith(".git"))value.chop(4);
    value.replace(QRegularExpression("[^\\p{L}\\p{N}._-]+"),"-");
    return value=="."||value==".."?QString():value.left(80);
}
QString CloneRepositoryDialog::settingsKey() const
{
    const QJsonArray source{m_client->executable(),m_host};
    return "workspace/repositoryClone/"+QString::fromLatin1(QJsonDocument(source).toJson(QJsonDocument::Compact).toHex());
}
QString CloneRepositoryDialog::destination() const
{
    auto parent=m_parent->text();while(parent.endsWith('/')&&parent.size()>1)parent.chop(1);
    return parent+(parent.endsWith('/')?QString():QStringLiteral("/"))+m_name->text();
}
CloneRepositoryDialog::CloneRepositoryDialog(HgsClient *client,const QString &host,const QString &machine,
    MachinePlatform::Style style,const SessionOrganization &projects,const QString &project,bool startSession,QWidget *parent)
    :QDialog(parent),m_client(client),m_host(host),m_style(style),m_startSession(startSession)
{
    setObjectName("cloneRepositoryDialog");setWindowTitle(tr("Clone repository"));setMinimumWidth(520);resize(640,530);
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(24,22,24,22);layout->setSpacing(14);
    auto label=[&](const QString &text){auto *l=new QLabel(text);l->setTextFormat(Qt::PlainText);l->setWordWrap(true);return l;};
    auto *heading=label(tr("Clone repository"));heading->setObjectName("heading");layout->addWidget(heading);
    auto *context=label(tr("Computer: %1").arg(machine));context->setObjectName("cloneComputer");layout->addWidget(context);
    auto *form=new QFormLayout;form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);form->setSpacing(12);
    m_url=new QLineEdit;m_url->setObjectName("cloneRepositoryUrl");m_url->setPlaceholderText("git@example.org:team/project.git");m_url->setMaxLength(4096);form->addRow(tr("Repository"),m_url);
    m_parent=new QLineEdit(QSettings().value(settingsKey()+"/parent","~").toString());m_parent->setObjectName("cloneParentFolder");
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
    connect(m_another,&QPushButton::clicked,this,[this]{
        if(m_request)return;
        QSettings settings;settings.setValue(settingsKey()+"/previous",settings.value(settingsKey()+"/pending"));settings.remove(settingsKey()+"/pending");settings.sync();
        m_uncertain=false;m_token.clear();m_expected.clear();m_created.clear();m_name->clear();m_customName=true;
        m_error->setText(tr("The original checkout is kept. Choose a different destination for a new clone."));updateForm();
    });
    connect(buttons,&QDialogButtonBox::rejected,this,&CloneRepositoryDialog::reject);
    connect(m_accept,&QPushButton::clicked,this,&CloneRepositoryDialog::begin);
    connect(m_check,&QPushButton::clicked,this,[this]{m_action="status";m_request=m_client->repositoryOperation(m_host,m_action,{},{},{},m_token);m_error->setText(tr("Checking the original clone result…"));updateForm();});
    connect(m_browse,&QPushButton::clicked,this,[this]{const auto path=DirectoryDialog::chooseDirectory(m_client->executable(),m_host,m_parent->text(),this);if(!path.isEmpty())m_parent->setText(path);});
    connect(m_url,&QLineEdit::textChanged,this,[this]{if(!m_customName)m_name->setText(suggestedName(m_url->text()));updateForm();});
    connect(m_name,&QLineEdit::textEdited,this,[this]{m_customName=true;});
    connect(m_name,&QLineEdit::textChanged,this,[this]{if(!m_customProject)m_projectName->setText(m_name->text());updateForm();});
    connect(m_projectName,&QLineEdit::textEdited,this,[this]{m_customProject=true;});
    connect(m_projectName,&QLineEdit::textChanged,this,[this]{updateForm();});
    connect(m_parent,&QLineEdit::textChanged,this,[this]{updateForm();});
    connect(m_project,&QComboBox::currentIndexChanged,this,[this]{updateForm();});
    connect(m_client,&HgsClient::repositoryFinished,this,&CloneRepositoryDialog::finish);
    const auto saved=QJsonDocument::fromJson(QSettings().value(settingsKey()+"/pending").toByteArray()).object();
    if(!saved.isEmpty()){
        m_url->setText(saved["url"].toString());m_parent->setText(saved["parent"].toString());m_name->setText(saved["name"].toString());m_token=saved["request_id"].toString();m_expected=saved["path"].toString();m_uncertain=true;
        m_error->setText(tr("A previous clone is unconfirmed. Check its original result before continuing."));
    }
    updateForm();m_url->setFocus();
}
QString CloneRepositoryDialog::projectId() const {return m_project->currentData().toString();}
QString CloneRepositoryDialog::newProjectName() const {return projectId().isEmpty()?m_projectName->text().trimmed():QString();}
void CloneRepositoryDialog::reject(){if(m_request&&m_action=="clone")return;m_request=0;QDialog::reject();}
void CloneRepositoryDialog::updateForm()
{
    const bool busy=m_request!=0,locked=busy||m_uncertain||!m_created.isEmpty();
    for(auto *w:QList<QWidget *>{m_url,m_parent,m_name,m_browse})w->setEnabled(!locked);
    m_project->setEnabled(!busy);m_projectName->setEnabled(!busy&&projectId().isEmpty());
    m_path->setText(MachinePlatform::native(destination(),m_style));
    const bool name=!m_name->text().isEmpty()&&!m_name->text().contains(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\:*?\"<>|]"))&&m_name->text()!="."&&m_name->text()!="..";
    m_accept->setEnabled(!busy&&!m_uncertain&&name&&!m_url->text().trimmed().isEmpty()&&!m_parent->text().isEmpty()&&(!projectId().isEmpty()||!newProjectName().isEmpty()));
    m_cancel->setEnabled(!busy||m_action!="clone");m_check->setVisible(m_uncertain);m_check->setEnabled(!busy);m_another->setVisible(m_uncertain);m_another->setEnabled(!busy);m_progress->setVisible(busy);
}
void CloneRepositoryDialog::begin()
{
    if(m_request||m_uncertain)return;
    if(!m_created.isEmpty()){accept();return;}
    m_action="check";m_error->setText(tr("Checking repository access and destination…"));
    m_request=m_client->repositoryOperation(m_host,m_action,m_url->text().trimmed(),m_parent->text(),m_name->text());updateForm();
}
void CloneRepositoryDialog::finish(quint64 request,bool ok,const QJsonObject &data,const QString &error)
{
    if(request!=m_request)return;m_request=0;
    if(!ok){if(m_action!="check")m_uncertain=true;m_error->setText(error);updateForm();return;}
    if(m_action=="check"){
        if(!MachinePlatform::absolute(data["parent"].toString(),m_style)
            ||!MachinePlatform::same(data["path"].toString(),MachinePlatform::clean(data["parent"].toString()+'/'+m_name->text(),m_style),m_style)){
            m_error->setText(tr("The computer did not confirm this destination."));updateForm();return;
        }
        m_parent->setText(data["parent"].toString());m_expected=data["path"].toString();
        m_token=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject pending{{"request_id",m_token},{"url",m_url->text().trimmed()},{"parent",m_parent->text()},{"name",m_name->text()},{"path",m_expected}};
        QSettings settings;settings.setValue(settingsKey()+"/pending",QJsonDocument(pending).toJson(QJsonDocument::Compact));settings.sync();
        if(settings.status()!=QSettings::NoError){m_error->setText(tr("Could not save clone recovery information."));updateForm();return;}
        m_action="clone";m_uncertain=true;m_error->setText(tr("Cloning repository…"));
        m_request=m_client->repositoryOperation(m_host,m_action,m_url->text().trimmed(),m_parent->text(),m_name->text(),m_token);updateForm();return;
    }
    if(!MachinePlatform::same(data["path"].toString(),m_expected,m_style)||data["name"]!=m_name->text()
        ||!MachinePlatform::same(data["parent"].toString(),m_parent->text(),m_style)){
        m_uncertain=true;m_error->setText(tr("Clone result does not match this destination. Inspect the folder before continuing."));updateForm();return;
    }
    const auto status=data["status"].toString();
    if(status=="cloned"||status=="failed"){
        QSettings settings;settings.remove(settingsKey()+"/pending");settings.setValue(settingsKey()+"/parent",m_parent->text());settings.sync();m_uncertain=false;
        if(status=="cloned"){
            m_created=data["path"].toString();
            if(m_action=="clone"){accept();return;}
            m_accept->setText(m_startSession?tr("Use checkout and start"):tr("Use checkout"));
            m_error->setText(tr("Checkout is ready. Confirm to use it."));updateForm();return;
        }
        m_error->setText(tr("%1\nAny remaining checkout files are kept. Choose another destination or use the existing folder.").arg(data["detail"].toString()));
    }else{m_uncertain=true;m_error->setText(tr("The original clone is still running or its completion is unknown. Check again; the operation will not be repeated."));}
    updateForm();
}
