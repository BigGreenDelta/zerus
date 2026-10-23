#include "RepositoryClone.h"
#include "HgsClient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>

QString RepositoryClone::suggestedName(const QString &url)
{
    auto value=url.trimmed();while(value.endsWith('/'))value.chop(1);
    value=value.mid(qMax(value.lastIndexOf('/'),value.lastIndexOf(':'))+1);
    if(value.endsWith(".git"))value.chop(4);
    value.replace(QRegularExpression("[^\\p{L}\\p{N}._-]+"),"-");
    return value=="."||value==".."?QString():value.left(80);
}
QString RepositoryClone::settingsKey() const
{
    const QJsonArray source{m_client->executable(),m_host};
    return "workspace/repositoryClone/"+QString::fromLatin1(QJsonDocument(source).toJson(QJsonDocument::Compact).toHex());
}
QString RepositoryClone::destination() const
{
    auto parent=m_parent;while(parent.endsWith('/')&&parent.size()>1)parent.chop(1);
    return parent+(parent.endsWith('/')?QString():QStringLiteral("/"))+m_name;
}
RepositoryClone::RepositoryClone(HgsClient *client,const QString &host,MachinePlatform::Style style,QObject *parent)
    :QObject(parent),m_client(client),m_host(host),m_style(style)
{
    connect(m_client,&HgsClient::repositoryFinished,this,&RepositoryClone::finish);
    m_parent=QSettings().value(settingsKey()+"/parent","~").toString();
    const auto saved=QJsonDocument::fromJson(QSettings().value(settingsKey()+"/pending").toByteArray()).object();
    if(!saved.isEmpty()){
        m_url=saved["url"].toString();m_parent=saved["parent"].toString();m_name=saved["name"].toString();
        m_token=saved["request_id"].toString();m_expected=saved["path"].toString();m_uncertain=true;
        m_status=tr("A previous clone is unconfirmed. Check its original result before continuing.");
    }
}
bool RepositoryClone::canSubmit() const
{
    const bool name=!m_name.isEmpty()&&!m_name.contains(QRegularExpression("[\\x00-\\x1f\\x7f/\\\\:*?\"<>|]"))&&m_name!="."&&m_name!="..";
    return !busy()&&!m_uncertain&&name&&!m_url.trimmed().isEmpty()&&!m_parent.isEmpty();
}
void RepositoryClone::setSource(const QString &url,const QString &parent,const QString &name)
{
    if(fieldsLocked()||(m_url==url&&m_parent==parent&&m_name==name))return;
    m_url=url;m_parent=parent;m_name=name;emit changed();
}
void RepositoryClone::cancelCheck(){if(!cloning()){m_request=0;emit changed();}}
void RepositoryClone::begin()
{
    if(!canSubmit())return;
    if(!m_created.isEmpty()){emit prepared(m_created);return;}
    m_action="check";m_status=tr("Checking repository access and destination…");
    m_request=m_client->repositoryOperation(m_host,m_action,m_url.trimmed(),m_parent,m_name);emit changed();
}
void RepositoryClone::checkResult()
{
    if(busy()||!m_uncertain)return;
    m_action="status";m_status=tr("Checking the original clone result…");
    m_request=m_client->repositoryOperation(m_host,m_action,{},{},{},m_token);emit changed();
}
void RepositoryClone::chooseAnotherDestination()
{
    if(busy()||!m_uncertain)return;
    QSettings settings;settings.setValue(settingsKey()+"/previous",settings.value(settingsKey()+"/pending"));settings.remove(settingsKey()+"/pending");settings.sync();
    m_uncertain=false;m_token.clear();m_expected.clear();m_created.clear();m_name.clear();
    m_status=tr("The original checkout is kept. Choose a different destination for a new clone.");emit changed();
}
void RepositoryClone::finish(quint64 request,bool ok,const QJsonObject &data,const QString &error)
{
    if(request!=m_request)return;m_request=0;
    if(!ok){if(m_action!="check")m_uncertain=true;m_status=error;emit changed();return;}
    if(m_action=="check"){
        if(!MachinePlatform::absolute(data["parent"].toString(),m_style)
            ||!MachinePlatform::same(data["path"].toString(),MachinePlatform::clean(data["parent"].toString()+'/'+m_name,m_style),m_style)){
            m_status=tr("The computer did not confirm this destination.");emit changed();return;
        }
        m_parent=data["parent"].toString();m_expected=data["path"].toString();m_token=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject pending{{"request_id",m_token},{"url",m_url.trimmed()},{"parent",m_parent},{"name",m_name},{"path",m_expected}};
        QSettings settings;settings.setValue(settingsKey()+"/pending",QJsonDocument(pending).toJson(QJsonDocument::Compact));settings.sync();
        if(settings.status()!=QSettings::NoError){m_status=tr("Could not save clone recovery information.");emit changed();return;}
        m_action="clone";m_uncertain=true;m_status=tr("Cloning repository…");
        m_request=m_client->repositoryOperation(m_host,m_action,m_url.trimmed(),m_parent,m_name,m_token);emit changed();return;
    }
    if(!MachinePlatform::same(data["path"].toString(),m_expected,m_style)||data["name"]!=m_name
        ||!MachinePlatform::same(data["parent"].toString(),m_parent,m_style)){
        m_uncertain=true;m_status=tr("Clone result does not match this destination. Inspect the folder before continuing.");emit changed();return;
    }
    const auto status=data["status"].toString();
    if(status=="cloned"||status=="failed"){
        QSettings settings;settings.remove(settingsKey()+"/pending");settings.setValue(settingsKey()+"/parent",m_parent);settings.sync();m_uncertain=false;
        if(status=="cloned"){
            m_created=data["path"].toString();
            if(m_action=="clone"){emit changed();emit prepared(m_created);return;}
            m_status=tr("Checkout is ready. Confirm to use it.");emit changed();return;
        }
        m_status=tr("%1\nAny remaining checkout files are kept. Choose another destination or use the existing folder.").arg(data["detail"].toString());
    }else{m_uncertain=true;m_status=tr("The original clone is still running or its completion is unknown. Check again; the operation will not be repeated.");}
    emit changed();
}
