#pragma once
#include "HgsClient.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <functional>

namespace AttachmentFiles {
inline QString root() {return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/attachments";}
// At most 180 characters. A longer name keeps its last extension, by which a
// viewer chooses the application: cutting the end could expose an inner one
// (A….jarX.command must not become A….jar). Without a short last extension
// the shortened name keeps no dot at all.
inline QString fitName(const QString &base) {
    constexpr int limit=180;
    if(base.size()<=limit)return base;
    const auto dot=base.lastIndexOf('.');
    if(dot>0 && base.size()-dot<=32)return base.left(limit-(base.size()-dot))+base.mid(dot);
    auto cut=base.left(limit);cut.replace('.','_');return cut;
}
inline QString store(const QString &name,const QByteArray &bytes) {
    if(bytes.isEmpty() || bytes.size()>HgsClient::MaximumAttachmentBytes)return {};
    const auto folder=root()+"/"+QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    if(!QDir().mkpath(folder))return {};
    QFile::setPermissions(root(),QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    QFile::setPermissions(folder,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    auto base=QFileInfo(name).fileName();base.replace(QRegularExpression("[^\\p{L}\\p{N}._ -]"),"_");
    if(base.isEmpty() || base=="." || base=="..")base="attachment";
    const auto path=folder+"/"+fitName(base);QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return {};
    file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
    if(file.write(bytes)!=bytes.size() || !file.commit())return {};return path;
}
inline QString cached(const QJsonObject &file) {
    const QFileInfo info(file.value("local_path").toString());const auto path=info.canonicalFilePath();
    const auto allowed=QFileInfo(root()).canonicalFilePath();
    return !allowed.isEmpty() && path.startsWith(allowed+'/') && info.isFile() && info.size()<=HgsClient::MaximumAttachmentBytes?path:QString();
}
}

// Fetches are scoped to an immutable machine/session/conversation/file identity.
// Preview and Open share both in-flight requests and the downloaded cache.
class AttachmentLoader : public QObject {
public:
    using Callback = std::function<void(const QString &, const QString &)>;
    explicit AttachmentLoader(HgsClient *client, QObject *parent=nullptr) : QObject(parent), m_client(client) {
        connect(client,&HgsClient::attachmentReady,this,[this](quint64 id,const QJsonObject &metadata,const QByteArray &bytes) {
            if(!m_pending.contains(id))return;
            const auto path=AttachmentFiles::store(metadata.value("name").toString(),bytes);
            finish(id,path,path.isEmpty()?tr("Could not save the attachment."):QString());
        });
        connect(client,&HgsClient::attachmentFailed,this,[this](quint64 id,const QString &error) { finish(id,{},error); });
    }
    void fetch(const QString &host,const QString &session,const QString &conversation,const QString &archive,
               const QString &agent,const QJsonObject &file,Callback done,bool priority=false) {
        const auto key=QString::fromUtf8(QJsonDocument(QJsonArray{host,session,conversation,archive,agent,file}).toJson(QJsonDocument::Compact));
        const auto local=AttachmentFiles::cached(file);
        const auto path=!local.isEmpty()?local:AttachmentFiles::cached({{"local_path",m_paths.value(key)}});
        if(!path.isEmpty()) { QTimer::singleShot(0,this,[done,path]{done(path,{});});return; }
        if(m_jobs.contains(key)) {
            m_jobs[key].callbacks.append(std::move(done));
            if(priority && m_queue.removeAll(key))m_queue.prepend(key);
            return;
        }
        if(file.value("request_id").toString().isEmpty()) {
            QTimer::singleShot(0,this,[done]{done({},tr("This attachment has no saved file reference."));});return;
        }
        m_jobs.insert(key,{host,session,conversation,archive,agent,file,{std::move(done)}});
        if(priority)m_queue.prepend(key);else m_queue.append(key);pump();
    }
    void open(const QString &host,const QString &session,const QString &conversation,const QString &archive,
              const QString &agent,const QJsonObject &file,Callback done) {
        fetch(host,session,conversation,archive,agent,file,[done](const QString &path,const QString &error) {
            if(!error.isEmpty()){done({},error);return;}
            if(!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {done({},tr("No application could open this file."));return;}
            done(path,{});
        },true);
    }
    static QImage thumbnail(const QString &path) {
        QImageReader reader(path);reader.setAutoTransform(true);
        const auto size=reader.size();
        if(!size.isValid() || qint64(size.width())*size.height()>64000000 || reader.format()=="svg")return {};
        reader.setScaledSize(size.scaled(QSize(192,128),Qt::KeepAspectRatio));
        return reader.read();
    }
private:
    struct Job { QString host,session,conversation,archive,agent; QJsonObject file; QList<Callback> callbacks; };
    void pump() {
        while(m_pending.size()<3 && !m_queue.isEmpty()) {
            const auto key=m_queue.takeFirst();const auto &job=m_jobs[key];
            const auto id=m_client->requestAttachment(job.host,job.session,job.file,job.conversation,job.archive,job.agent);
            m_pending.insert(id,key);
        }
    }
    void finish(quint64 id,const QString &path,const QString &error) {
        if(!m_pending.contains(id))return;
        const auto key=m_pending.take(id);const auto job=m_jobs.take(key);
        if(!path.isEmpty()) {if(m_paths.size()>=256)m_paths.erase(m_paths.begin());m_paths.insert(key,path);}
        for(const auto &done:job.callbacks)done(path,error);
        pump();
    }
    HgsClient *m_client;
    QHash<QString,QString> m_paths;
    QStringList m_queue;
    QHash<QString,Job> m_jobs;
    QHash<quint64,QString> m_pending;
};
