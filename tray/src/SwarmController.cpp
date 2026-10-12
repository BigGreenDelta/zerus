#include "SwarmController.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QSet>

namespace {
QJsonObject sharedOrganization(QJsonObject local)
{
    // Keep fallback placement/order in the desktop cache, but never send it as
    // membership. Older CLIs ignore the additive organization hint; the patch
    // envelope and shared catalog operation schema stay intact.
    QSet<QString> unassigned;
    for (const auto &session : local.take("unassigned_sessions").toArray()) unassigned.insert(session.toString());
    QJsonArray projects;
    for (const auto &value : local.value("projects").toArray()) {
        auto project = value.toObject(); QJsonArray sessions;
        for (const auto &session : project.value("sessions").toArray())
            if (!unassigned.contains(session.toString())) sessions.append(session);
        project.insert("sessions", sessions); projects.append(project);
    }
    local.insert("projects", projects);
    local.insert("session_assignments_explicit", true);
    return local;
}
}

SwarmController::SwarmController(HgsClient *client, QObject *parent) : QObject(parent), m_client(client)
{
    m_poll.setInterval(5000);
    connect(&m_poll,&QTimer::timeout,this,&SwarmController::refresh);
    m_flush.setSingleShot(true);m_flush.setInterval(120);
    connect(&m_flush,&QTimer::timeout,this,[this]{if(!m_suspended && !busy() && m_ready && m_base!=m_desired)send("apply");});
    connect(client,&HgsClient::swarmReady,this,&SwarmController::received);
    connect(client,&HgsClient::swarmFailed,this,[this](quint64 id,const QString &error){
        if(id!=m_request)return;m_request=0;
        emit statusChanged(tr("Project sync unavailable: %1").arg(error),true);
        // Keep both the draft and its observed versions on disk. A local CLI
        // failure must not turn the next read into an overwrite of unsaved edits.
        persist();
    });
}

void SwarmController::start(const QJsonObject &organization)
{
    if(m_started)return;m_started=true;m_desired=organization;
    QSettings settings;
    if(!settings.contains("workspace/organizationBeforeSwarm"))settings.setValue("workspace/organizationBeforeSwarm",QJsonDocument(organization).toJson(QJsonDocument::Compact));
    m_base=QJsonDocument::fromJson(settings.value("swarm/observedOrganization").toByteArray()).object();
    m_versions=QJsonDocument::fromJson(settings.value("swarm/observedVersions").toByteArray()).object();
    m_ready=!m_base.isEmpty();
    if(!m_ready)m_base=organization;
    m_poll.start();
    // initialize is idempotent and imports only once. Existing catalogs are read
    // without replaying the GUI cache; pending drafts retain their own baseline.
    send(m_ready?"get":"initialize");
}
void SwarmController::edit(const QJsonObject &organization)
{
    m_desired=organization;
    if(!m_started)return;
    if(!busy() && m_ready)m_flush.start();
}
void SwarmController::refresh()
{
    if(!m_started||busy()||m_suspended)return;
    if(m_draftEditing && m_base==m_desired)return;
    send(!m_ready?"initialize":m_base!=m_desired?"apply":"get");
}
void SwarmController::send(const QString &action)
{
    if(busy())return;
    m_action=action;m_sent=m_desired;
    QJsonObject input;
    if(action=="initialize")input=sharedOrganization(m_desired);
    if(action=="apply")input={{"base",sharedOrganization(m_base)},{"desired",sharedOrganization(m_desired)},{"versions",m_versions}};
    m_request=m_client->requestSwarm({action},input);
}
void SwarmController::persist()
{
    if (!m_ready) return;
    const auto organization = QJsonDocument(m_base).toJson(QJsonDocument::Compact);
    const auto versions = QJsonDocument(m_versions).toJson(QJsonDocument::Compact);
    // Polls arrive even when the shared catalog is unchanged. Rewriting its
    // settings would synchronously wait for storage on the GUI thread each
    // time. Remember only successful checkpoints, so a failed write is retried.
    if (organization == m_persistedOrganization && versions == m_persistedVersions) return;
    QSettings settings;
    settings.setValue("swarm/observedOrganization", organization);
    settings.setValue("swarm/observedVersions", versions);
    settings.sync();
    if (settings.status() == QSettings::NoError) {
        m_persistedOrganization = organization;
        m_persistedVersions = versions;
    }
}
QJsonObject SwarmController::presentation(const QJsonObject &shared, const QJsonObject &local)
{
    QMap<QString,QJsonObject> remaining;
    QSet<QString> assigned, unassigned, retained;
    for(const auto &p:shared.value("projects").toArray()) {
        remaining.insert(p.toObject().value("id").toString(),p.toObject());
        for(const auto &session:p.toObject().value("sessions").toArray())assigned.insert(session.toString());
    }
    for(const auto &session:local.value("unassigned_sessions").toArray())unassigned.insert(session.toString());
    QJsonArray ordered;
    for(const auto &p:local.value("projects").toArray()){
        const auto old=p.toObject();const auto id=old.value("id").toString();
        if(!remaining.contains(id))continue;
        auto project=remaining.take(id);project.insert("collapsed",old.value("collapsed"));project.insert("vivid",old.value("vivid"));
        // Preserve local fallback rows only until the catalog assigns them.
        auto sessions=project.value("sessions").toArray();QJsonArray sorted;
        for(const auto &session:old.value("sessions").toArray())
            if(unassigned.contains(session.toString()) && !assigned.contains(session.toString())) {
                sessions.append(session);retained.insert(session.toString());
            }
        // Session order is local too. Explicit membership comes from the catalog.
        for(const auto &s:old.value("sessions").toArray())if(sessions.contains(s)){sorted.append(s);for(qsizetype i=0;i<sessions.size();++i)if(sessions[i]==s){sessions.removeAt(i);break;}}
        for(const auto &s:sessions)sorted.append(s);project.insert("sessions",sorted);ordered.append(project);
    }
    for(const auto &project:remaining)ordered.append(project);
    auto result=shared;result.insert("projects",ordered);
    if(local.contains("unassigned_sessions")) {
        QStringList ids(retained.cbegin(),retained.cend());ids.sort();
        result.insert("unassigned_sessions",QJsonArray::fromStringList(ids));
    }
    for(const auto &key:{"default_project","runs","imported_machines"})if(local.contains(key))result.insert(key,local.value(key));
    return result;
}
void SwarmController::received(quint64 id,const QJsonObject &result)
{
    if(id!=m_request)return;m_request=0;
    if(result.value("schema").toInt()!=1 || !result.value("organization").isObject()){
        emit statusChanged(tr("Project sync needs an updated HGS installation."),true);return;
    }
    const bool previouslyReady=m_ready;m_ready=true;
    const auto written=result.value("written").toObject();
    // Advancing only our own acknowledged fields preserves genuine conflicts with
    // remote changes that the user has not seen while editing the same field.
    for(auto it=written.begin();it!=written.end();++it)m_versions.insert(it.key(),it.value());
    if(m_action=="apply" || (m_action=="initialize" && !previouslyReady))m_base=m_sent;
    const bool pending=m_desired!=m_base;
    if(pending){persist();m_flush.start();return;}
    if(m_draftEditing){persist();return;}
    m_snapshot=result;m_versions=result.value("versions").toObject();
    m_desired=presentation(result.value("organization").toObject(),m_desired);
    m_base=m_desired;persist();
    emit organizationReady(m_desired);
    emit snapshotChanged(result);
    const int conflicts=result.value("conflicts").toArray().size();
    const auto peers=result.value("peers").toObject();
    int offline=0;for(const auto &p:peers)if(!p.toObject().value("error").toString().isEmpty())++offline;
    QString status=peers.isEmpty()?tr("Local catalog. Connect another computer to sync projects."):tr("Project sync connected to %n computer(s)",nullptr,peers.size());
    if(offline)status=tr("%n connection(s) unavailable. Changes are saved here and will sync when reachable.",nullptr,offline);
    if(conflicts)status=tr("%n project conflict(s) need review in Swarm.",nullptr,conflicts);
    emit statusChanged(status,conflicts>0 || offline>0);
}
void SwarmController::acceptExternal(const QJsonObject &snapshot)
{
    if(busy() || !snapshot.value("organization").isObject())return;
    m_base=m_desired;m_action="get";
    received(0,snapshot);
}
