#pragma once
#include "ExternalFile.h"
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
#include <memory>
#include <QFutureWatcher>
#include <QPromise>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QMutex>
#include <QMutexLocker>

namespace AttachmentFiles {
inline QString root() {return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)+"/attachments";}
// Capture the settings-derived root on the caller's thread. Workers receive
// only immutable values and never query application settings or UI objects.
inline QString storeAt(const QString &cacheRoot, const QString &name, const QByteArray &bytes) {
    if(bytes.isEmpty() || bytes.size()>HgsClient::MaximumAttachmentBytes)return {};
    const auto digest=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    if(!QDir().mkpath(cacheRoot))return {};
    const auto canonicalRoot=QFileInfo(cacheRoot).canonicalFilePath();
    const auto folder=cacheRoot+"/"+digest;
    if(!QDir().mkpath(folder) || QFileInfo(folder).canonicalFilePath()!=canonicalRoot+'/'+digest)return {};
    QFile::setPermissions(cacheRoot,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    QFile::setPermissions(folder,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    auto base=QFileInfo(name).fileName();base.replace(QRegularExpression("[^\\p{L}\\p{N}._ -]"),"_");
    // Cached copies stay plain files everywhere. Linux and macOS limit names to 255
    // bytes, and QSaveFile may add a 7-byte temporary suffix; Windows drops trailing
    // dots and spaces and reserves device names; Qt and the Windows shell treat a
    // .lnk name as a shortcut to another path. Shortening keeps the extension, which
    // chooses the application that opens the copy, and never splits a surrogate pair.
    const auto dot=base.lastIndexOf('.');const auto extension=dot>0 && base.size()-dot<=16?base.mid(dot):QString();
    auto stem=base.left(base.size()-extension.size());
    while(!stem.isEmpty() && (stem.size()+extension.size()>180 || QFile::encodeName(stem+extension).size()>240)) {
        stem.chop(1);if(!stem.isEmpty() && stem.back().isHighSurrogate())stem.chop(1);
    }
    base=stem+extension;while(base.endsWith('.') || base.endsWith(' '))base.chop(1);
    if(base.isEmpty())base="attachment";
    static const QRegularExpression device("^(con|prn|aux|nul|com[0-9\\x{b9}\\x{b2}\\x{b3}]|lpt[0-9\\x{b9}\\x{b2}\\x{b3}]) *(\\..*)?$",
        QRegularExpression::CaseInsensitiveOption);
    if(device.match(base).hasMatch())base.prepend('_');
    if(base.endsWith(".lnk",Qt::CaseInsensitive))base+='_';
    // Identical attachments share this content-addressed path. Writers take turns
    // and keep an intact copy: Windows cannot replace a file that another worker,
    // a thumbnail reader or an opened viewer has open.
    static QBasicMutex writers;const QMutexLocker lock(&writers);
    const auto path=folder+"/"+base;const QFileInfo entry(path);
    // Only a plain file is reused; a link of any kind leads elsewhere.
    if(QFile existing(path);!entry.isSymLink() && entry.isFile() && existing.open(QIODevice::ReadOnly)
        && existing.size()==bytes.size() && existing.read(bytes.size()+1)==bytes)return path;
    // QSaveFile writes through links, shortcuts and aliases, so remove the entry itself.
    if(entry.isSymLink() && !QFile::remove(path))return {};
    // Windows also refuses the replacement while another process, such as a virus
    // scanner, the search indexer or a viewer, briefly holds the old copy open:
    // this worker retries for a while before it gives up.
    for(int attempt=0;;++attempt) {
        QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))return {};
        file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        if(file.write(bytes)==bytes.size() && file.commit())return path;
#ifdef Q_OS_WIN
        if(attempt<40) { QThread::msleep(50); continue; }
#endif
        return {};
    }
}
inline QString store(const QString &name,const QByteArray &bytes) { return storeAt(root(),name,bytes); }
inline QString cachedAt(const QString &cacheRoot, const QJsonObject &file) {
    const QFileInfo info(file.value("local_path").toString());const auto path=info.canonicalFilePath();
    const auto allowed=QFileInfo(cacheRoot).canonicalFilePath();
    return !allowed.isEmpty() && path.startsWith(allowed+'/') && info.isFile() && info.size()>0
        && info.size()<=HgsClient::MaximumAttachmentBytes?path:QString();
}
inline QString cached(const QJsonObject &file) { return cachedAt(root(),file); }
inline QThreadPool &pool() {
    static QThreadPool value;
    static const bool configured=[] { value.setMaxThreadCount(2); return true; }();
    Q_UNUSED(configured); return value;
}
inline QMutex &observerMutex() { static QMutex value; return value; }
inline std::function<void()> &ioObserver() { static std::function<void()> value; return value; }
// Deterministic tests can hold disk work without blocking the GUI thread.
inline void setIoObserver(std::function<void()> observer) { QMutexLocker lock(&observerMutex()); ioObserver()=std::move(observer); }
inline std::function<void()> capturedObserver() { QMutexLocker lock(&observerMutex()); return ioObserver(); }
template<class Result, class Work, class Done>
inline void runAsync(QObject *owner, Work work, Done done) {
    if(!owner)return;
    auto promise=std::make_shared<QPromise<Result>>();promise->start();
    auto *watcher=new QFutureWatcher<Result>(owner);
    QObject::connect(watcher,&QFutureWatcher<Result>::finished,owner,[watcher,done=std::move(done)]() mutable {
        const auto result=watcher->result();watcher->deleteLater();done(result);
    });
    watcher->setFuture(promise->future());
    pool().start(QRunnable::create([promise,work=std::move(work)]() mutable {
        promise->addResult(work());promise->finish();
    }));
}
struct Prepared { QJsonArray files; QString error; };
inline void prepareAsync(const QList<MessageAttachment> &attachments, QObject *owner, std::function<void(QJsonArray,QString)> done) {
    if(attachments.isEmpty()) { if(owner)QTimer::singleShot(0,owner,[done=std::move(done)]{done({},{});}); return; }
    const auto cacheRoot=root();const auto observer=capturedObserver();
    runAsync<Prepared>(owner,[attachments,cacheRoot,observer] {
        if(observer)observer();
        const auto failure=[] { return Prepared{{},QObject::tr("Could not save the attachments. Your draft was kept without sending.")}; };
        if(attachments.size()>HgsClient::MaximumAttachments)return failure();
        qsizetype total=0;
        for(const auto &attachment:attachments) {
            if(attachment.data.isEmpty() || attachment.data.size()>HgsClient::MaximumAttachmentBytes)return failure();
            total+=attachment.data.size();if(total>HgsClient::MaximumAttachmentsBytes)return failure();
        }
        Prepared result;
        for(const auto &attachment:attachments) {
            const auto path=storeAt(cacheRoot,attachment.name,attachment.data);if(path.isEmpty())return failure();
            result.files.append(QJsonObject{{"name",attachment.name},{"mime",attachment.mime},{"bytes",qint64(attachment.data.size())},
                {"reference",attachment.reference},{"local_path",path}});
        }
        return result;
    },[done=std::move(done)](const Prepared &result) {done(result.files,result.error);});
}
struct Read { QList<MessageAttachment> attachments; QString error; };
inline void readAsync(const QJsonArray &files, QObject *owner, std::function<void(QList<MessageAttachment>,QString)> done) {
    if(files.isEmpty()) { if(owner)QTimer::singleShot(0,owner,[done=std::move(done)]{done({},{});}); return; }
    const auto cacheRoot=root();const auto observer=capturedObserver();
    runAsync<Read>(owner,[files,cacheRoot,observer] {
        if(observer)observer();
        const auto failure=[] { return Read{{},QObject::tr("The saved attachment is unavailable or changed. Attach it again before sending.")}; };
        if(files.size()>HgsClient::MaximumAttachments)return failure();
        Read result;qsizetype total=0;
        for(const auto &value:files) {
            if(!value.isObject())return failure();
            const auto metadata=value.toObject();const auto path=cachedAt(cacheRoot,metadata);
            QFile file(path);if(path.isEmpty() || !file.open(QIODevice::ReadOnly))return failure();
            // Bound the allocation even if the file grows after its initial stat.
            const auto limit=qMin(HgsClient::MaximumAttachmentBytes,HgsClient::MaximumAttachmentsBytes-total);
            const auto bytes=file.read(limit+1);
            if(file.error()!=QFileDevice::NoError || bytes.isEmpty() || bytes.size()>limit)return failure();
            total+=bytes.size();if(total>HgsClient::MaximumAttachmentsBytes)return failure();
            if(!metadata.value("bytes").isDouble() || metadata.value("bytes").toDouble(-1)!=bytes.size())return failure();
            const auto digest=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
            if(QFileInfo(path).dir().dirName()!=digest)return failure();
            result.attachments.append({metadata.value("name").toString(),metadata.value("mime").toString(),bytes,metadata.value("reference").toString()});
        }
        return result;
    },[done=std::move(done)](const Read &result) {done(result.attachments,result.error);});
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
            if(m_saving.contains(id))return;
            m_saving.insert(id);
            AttachmentFiles::prepareAsync({MessageAttachment{metadata.value("name").toString(),metadata.value("mime").toString(),bytes,{}}},this,
                [this,id](const QJsonArray &files,const QString &error) {
                    m_saving.remove(id);
                    const auto path=files.isEmpty()?QString():files.first().toObject().value("local_path").toString();
                    finish(id,path,error.isEmpty()?QString():tr("Could not save the attachment."));
                });
        });
        connect(client,&HgsClient::attachmentFailed,this,[this](quint64 id,const QString &error) { if(!m_saving.contains(id))finish(id,{},error); });
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
    // Conversation attachments come from another machine, like browser downloads:
    // `done` receives the path and either an error or an informational notice.
    using OpenCallback = std::function<void(const QString &path, const QString &error, const QString &notice)>;
    void open(const QString &host,const QString &session,const QString &conversation,const QString &archive,
              const QString &agent,const QJsonObject &file,OpenCallback done) {
        fetch(host,session,conversation,archive,agent,file,[done](const QString &path,const QString &error) {
            if(!error.isEmpty()){done({},error,{});return;}
            switch(ExternalFile::open(path,ExternalFile::Origin::OtherMachine)) {
            case ExternalFile::Outcome::Opened: done(path,{},{});return;
            case ExternalFile::Outcome::Revealed: done(path,{},ExternalFile::revealedNotice(path));return;
            case ExternalFile::Outcome::RevealFailed: done({},ExternalFile::revealFailedNotice(path),{});return;
            case ExternalFile::Outcome::OpenFailed: done({},tr("No application could open this file."),{});return;
            }
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
        m_saving.remove(id);
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
    QSet<quint64> m_saving;
};
