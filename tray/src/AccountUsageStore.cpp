#include "AccountUsageStore.h"
#include <QDateTime>

AccountUsageStore::AccountUsageStore(const QString &hgs,QObject *parent,std::function<qint64()> clock):QObject(parent),m_client(hgs,this),m_now(clock?clock:QDateTime::currentMSecsSinceEpoch)
{
    connect(&m_client,&HgsClient::accountsReady,this,[this](quint64 id,const QString &,const QJsonObject &data){finish(id,data);});
    connect(&m_client,&HgsClient::accountsFailed,this,[this](quint64 id,const QString &,const QString &){finish(id,{});});
}
bool AccountUsageStore::online(const QString &host) const
{
    if(host.isEmpty())return true;
    const auto *box=m_fleet.peer(host);
    // Explicitly opened profiles can belong to a machine excluded from polling.
    return !box || m_fleet.peerPolledAt(host)<=0 || box->ok;
}
QJsonObject AccountUsageStore::data(const AccountUsageRef &ref) const
{
    const auto entry=m_entries.value(ref.key());auto data=entry.data;
    if(!ref.authRevision.isEmpty()&&ref.authRevision!=entry.ref.authRevision)data={};
    if(data.isEmpty())data={{"status",online(ref.host)?"loading":"offline"},{"provider",ref.provider},{"id",ref.id}};
    if(!ref.label.isEmpty())data["label"]=ref.label;
    data["refreshing"]=entry.pending;data["refresh_error"]=entry.failed;data["offline"]=!online(ref.host);
    return data;
}
void AccountUsageStore::ensure(const AccountUsageRef &ref,bool force)
{
    if(!ref.valid())return;
    auto &entry=m_entries[ref.key()];
    if(entry.pending)return;
    const bool authChanged=!ref.authRevision.isEmpty() && ref.authRevision!=entry.ref.authRevision;
    const auto revision=entry.ref.authRevision;
    if(authChanged && !revision.isEmpty())entry.data={};
    entry.ref=ref;
    if(entry.ref.authRevision.isEmpty())entry.ref.authRevision=revision;
    const auto now=m_now();
    if(entry.pending || !online(ref.host) || (!force && !authChanged && entry.attempted && now-entry.attempted<RefreshInterval))return;
    entry.attempted=now;entry.pending=true;m_queue.enqueue(ref.key());emit changed(ref.key());dispatch();
}
void AccountUsageStore::dispatch()
{
    while(m_pending.size()<2 && !m_queue.isEmpty()) {
        const auto key=m_queue.dequeue();const auto ref=m_entries.value(key).ref;
        if(!online(ref.host)){m_entries[key].pending=false;emit changed(key);continue;}
        QStringList args{"inspect"};
        if(ref.session.isEmpty())args<<ref.id;else args<<"--session"<<ref.session;
        args<<"--refresh";
        const auto request=m_client.requestAccounts(ref.host,args);m_pending[request]=ref;
    }
}
void AccountUsageStore::finish(quint64 request,const QJsonObject &data)
{
    if(!m_pending.contains(request))return;
    const auto ref=m_pending.take(request);auto &entry=m_entries[ref.key()];entry.pending=false;
    const bool matches=!data.isEmpty() && data.contains("status")
        && (ref.id.isEmpty() || data["id"]==ref.id)
        && (ref.home.isEmpty() || data["home"]==ref.home)
        && (data["provider"].toString().isEmpty() || data["provider"]==ref.provider)
        && (ref.session.isEmpty() || data["run_id"].toString().isEmpty() || data["run_id"]==ref.run);
    const bool failed=!matches || data["status"]=="unavailable";
    entry.failed=failed;
    if(matches && (!failed || entry.data.isEmpty()))entry.data=data;
    else if(entry.data.isEmpty())entry.data={{"id",ref.id},{"provider",ref.provider},{"status","unavailable"}};
    emit changed(ref.key());dispatch();
}
