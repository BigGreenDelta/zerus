#pragma once
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUuid>
#include <optional>
#include <cerrno>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RelaySettings {
inline QString canonicalRelay(){return QStringLiteral("https://relay.zerus.dev");}
inline QString configPath(){
#ifdef Q_OS_WIN
    const auto explicitRoot=qEnvironmentVariable("HGS_CONFIG_DIR");
    if(QDir::isAbsolutePath(explicitRoot))return explicitRoot+"/mobile/connector.json";
    const auto home=qEnvironmentVariable("HOME");
    return (QDir::isAbsolutePath(home)?home:QDir::homePath())+"/.config/hgs/mobile/connector.json";
#endif
    auto root=qEnvironmentVariable("XDG_CONFIG_HOME");
    if(!QDir::isAbsolutePath(root))root=QDir::homePath()+"/.config";
    return root+"/hgs/mobile/connector.json";
}
inline QString defaultStateDirectory(){return QDir::homePath()+"/.local/state/hgs/mobile-connector";}
inline QString newStateDirectory(){return defaultStateDirectory()+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces);}
inline QString normalizedUrl(QString text){while(text.endsWith('/'))text.chop(1);return text;}
inline bool validUrl(const QString &text){
    if(text.isEmpty()||text.size()>2048)return false;
    for(const auto c:text)if(c.unicode()<33||c.unicode()==127)return false;
    const QUrl u(text,QUrl::StrictMode);
    return u.isValid()&&u.scheme()=="https"&&!u.host().isEmpty()&&!u.authority(QUrl::FullyEncoded).contains('@')&&u.userName().isEmpty()
        &&u.password().isEmpty()&&!u.hasQuery()&&!u.hasFragment();
}
inline QString managedOrigin(const QString &text){
    if(!validUrl(text))return {};
    const QUrl u(text,QUrl::StrictMode);
    if(u.port(443)!=443||(!u.path().isEmpty()&&u.path()!="/"))return {};
    if(u.host()=="relay.zerus.dev")return canonicalRelay();
    if(u.host()=="zerus.dev.guthub.dev")return QStringLiteral("https://zerus.dev.guthub.dev");
    return {};
}
inline bool validToken(const QString &token){
    if(token.isEmpty()||token.size()>8192)return false;
    for(const auto c:token)if(c.unicode()<33||c.unicode()>126)return false;
    return true;
}
struct Snapshot {
    bool exists=false;
    QJsonObject object;
    QByteArray bytes;
    quint64 device=0,inode=0;
};
struct Edit {
    QString relay,replacementToken,stateDirectory;
};
struct Prepared {
    QJsonObject object;
    bool newState=false;
};
inline std::optional<Prepared> prepare(const Snapshot &old,const Edit &edit,QString *error){
    auto fail=[&](const QString &message)->std::optional<Prepared>{if(error)*error=message;return {};};
    const auto relay=normalizedUrl(edit.relay);
    if(!validUrl(relay))return fail(QStringLiteral("Enter an HTTPS relay URL without credentials, query or fragment."));
    if(managedOrigin(relay)=="https://zerus.dev.guthub.dev")
        return fail(QStringLiteral("The previous official relay is retiring. Use https://relay.zerus.dev."));
    const auto previous=normalizedUrl(old.object.value("server_url").toString());
    const auto oldToken=old.object.value("node_token").toString();
    const auto token=edit.replacementToken.isEmpty()?oldToken:edit.replacementToken;
    if(!validToken(token))return fail(QStringLiteral("Supply the computer credential issued by this relay."));
    const auto identityValue=old.object.value("identity_url");
    if(old.exists&&!identityValue.isUndefined()&&!identityValue.isNull()&&!identityValue.isString())
        return fail(QStringLiteral("The existing receipt identity must be a URL string. Review the connector configuration before saving."));
    const auto previousIdentity=normalizedUrl(identityValue.toString(previous));
    if(old.exists&&(!validUrl(previous)||!validToken(oldToken)))
        return fail(QStringLiteral("The existing configuration is invalid. Correct it using the connector setup documentation."));
    if(old.exists&&previousIdentity!=previous&&!(managedOrigin(previous)==canonicalRelay()
        &&managedOrigin(previousIdentity)=="https://zerus.dev.guthub.dev"))
        return fail(QStringLiteral("The existing receipt identity is invalid. Review the connector configuration before saving."));
    const bool equivalent=old.exists&&(relay==previous||(!managedOrigin(relay).isEmpty()
        &&!managedOrigin(previous).isEmpty()));
    const bool sameCredential=token==oldToken;
    const bool newState=!old.exists||!equivalent||!sameCredential;
    if(!newState&&previousIdentity!=relay&&!(managedOrigin(relay)==canonicalRelay()
        &&managedOrigin(previousIdentity)=="https://zerus.dev.guthub.dev"))
        return fail(QStringLiteral("Keep this relay URL's existing spelling to preserve its receipt journal. A new binding requires a replacement credential and a new receipt directory."));
    if(old.exists&&!equivalent&&(edit.replacementToken.isEmpty()||sameCredential))
        return fail(QStringLiteral("A different relay requires its own new computer credential. Existing credentials cannot be reused."));
    auto state=edit.stateDirectory;
    if(!QDir::isAbsolutePath(state)||QDir::cleanPath(state)!=state)
        return fail(QStringLiteral("Choose an absolute private receipt directory without relative path segments."));
    auto priorState=old.object.value("state_dir").toString(defaultStateDirectory());
    if(priorState.isEmpty())priorState=defaultStateDirectory();
    if(newState&&(state==priorState||QFileInfo::exists(state)||QFileInfo(state).isSymLink()))
        return fail(QStringLiteral("Use a new receipt directory. The previous journal must remain untouched."));
    if(!newState&&state!=priorState)
        return fail(QStringLiteral("Keep the receipt directory unchanged for this relay and credential."));
    auto out=old.object;
    out.insert("server_url",relay);out.insert("node_token",token);out.insert("state_dir",state);
    // Keep the original string spelling used by Python's URL/token namespace hash.
    if(!newState&&managedOrigin(relay)==canonicalRelay()&&managedOrigin(previousIdentity)=="https://zerus.dev.guthub.dev")
        out.insert("identity_url",previousIdentity);
    else if(!newState&&previousIdentity==relay)out.remove("identity_url");
    else if(newState)out.remove("identity_url");
    if(!old.exists){out.insert("hgs_path","hgs");out.insert("poll_interval",5);}
    return Prepared{out,newState};
}

// Credentials never enter QSettings, diagnostics or process arguments. A held
// directory descriptor and no-follow opens reject symlinks at every path level.
class ConfigStore {
public:
    explicit ConfigStore(QString path=configPath()):path_(std::move(path)){}
    QString path()const{return path_;}
    bool load(Snapshot *snapshot,QString *error)const{
#ifdef Q_OS_UNIX
        const auto parent=QFileInfo(path_).absolutePath();
        if(!QFileInfo::exists(parent)&&!QFileInfo(parent).isSymLink()){*snapshot={};return true;}
        int dir=openDirectory(parent,false,error);if(dir<0)return false;
        const bool ok=readAt(dir,QFileInfo(path_).fileName().toUtf8(),snapshot,error);
        ::close(dir);return ok;
#else
        Q_UNUSED(snapshot);if(error)*error=QStringLiteral("Private connector configuration is supported on Unix systems.");return false;
#endif
    }
    bool save(const Snapshot &expected,const Prepared &prepared,Snapshot *saved,QString *error)const{
#ifdef Q_OS_UNIX
        auto fail=[&](const QString &text){if(error)*error=text;return false;};
        int dir=openDirectory(QFileInfo(path_).absolutePath(),true,error);if(dir<0)return false;
        const auto name=QFileInfo(path_).fileName().toUtf8();
        Snapshot current;
        if(!readAt(dir,name,&current,error)){::close(dir);return false;}
        auto same=[&](const Snapshot &v){return v.exists==expected.exists&&v.bytes==expected.bytes
            &&v.device==expected.device&&v.inode==expected.inode;};
        if(!same(current)){::close(dir);return fail(QStringLiteral("Configuration changed outside this editor. Reload before saving."));}
        const auto bytes=QJsonDocument(prepared.object).toJson();
        if(bytes.size()>65536){::close(dir);return fail(QStringLiteral("Configuration exceeds the supported size."));}
        int state=-1;
        if(prepared.newState){
            const auto statePath=prepared.object.value("state_dir").toString();
            const auto stateParent=QFileInfo(statePath).absolutePath();
            int parent=openDirectory(stateParent,true,error,false);
            if(parent<0){::close(dir);return false;}
            const auto child=QFileInfo(statePath).fileName().toUtf8();
            if(::mkdirat(parent,child.constData(),0700)!=0){::close(parent);::close(dir);return fail(QStringLiteral("Cannot allocate a new private receipt directory."));}
            state=parent;
        }
        const auto tmp=QByteArray(".connector-")+QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8()+".tmp";
        int fd=::openat(dir,tmp.constData(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        bool ok=fd>=0;
        qsizetype offset=0;
        while(ok&&offset<bytes.size()){
            const auto n=::write(fd,bytes.constData()+offset,bytes.size()-offset);
            if(n<=0)ok=false;else offset+=n;
        }
        if(ok)ok=::fsync(fd)==0;
        if(fd>=0)::close(fd);
        Snapshot last;
        if(ok)ok=readAt(dir,name,&last,error)&&same(last);
        bool renamed=false;
        if(ok){renamed=::renameat(dir,tmp.constData(),dir,name.constData())==0;ok=renamed;}
        if(ok)ok=::fsync(dir)==0;
        if(!ok)::unlinkat(dir,tmp.constData(),0);
        if(state>=0){
            if(!renamed)::unlinkat(state,QFileInfo(prepared.object.value("state_dir").toString()).fileName().toUtf8().constData(),AT_REMOVEDIR);
            ::fsync(state);::close(state);
        }
        if(!ok){::close(dir);return fail(QStringLiteral("Configuration was not confirmed saved. Reload to check; no connector restart was requested."));}
        ok=readAt(dir,name,saved,error);::close(dir);return ok;
#else
        Q_UNUSED(expected);Q_UNUSED(prepared);Q_UNUSED(saved);if(error)*error=QStringLiteral("Private connector configuration is supported on Unix systems.");return false;
#endif
    }
private:
    QString path_;
#ifdef Q_OS_UNIX
    static int openDirectory(const QString &path,bool create,QString *error,bool privateFinal=true){
        auto fail=[&](){if(error)*error=QStringLiteral("Configuration directories must be owned, private and free of symlinks.");return -1;};
        if(!QDir::isAbsolutePath(path)||QDir::cleanPath(path)!=path)return fail();
        int fd=::open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return fail();
        const auto parts=path.split('/',Qt::SkipEmptyParts);
        for(int i=0;i<parts.size();++i){
            const auto part=parts[i].toUtf8();
            int next=::openat(fd,part.constData(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if(next<0&&errno==ENOENT&&create){
                if(::mkdirat(fd,part.constData(),0700)==0){::fsync(fd);next=::openat(fd,part.constData(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
            }
            ::close(fd);if(next<0)return fail();fd=next;
            struct stat st{};
            if(::fstat(fd,&st)!=0||(st.st_uid!=::geteuid()&&st.st_uid!=0)
                ||((st.st_mode&0022)&&!(st.st_uid==0&&(st.st_mode&S_ISVTX)))
                ||(i==parts.size()-1&&(st.st_uid!=::geteuid()||(privateFinal&&(st.st_mode&0077))))){::close(fd);return fail();}
        }
        return fd;
    }
    static bool readAt(int dir,const QByteArray &name,Snapshot *out,QString *error){
        auto fail=[&](const QString &text){if(error)*error=text;return false;};
        int fd=::openat(dir,name.constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(fd<0){if(errno==ENOENT){*out={};return true;}return fail(QStringLiteral("Cannot read private connector configuration; symlinks are refused."));}
        struct stat st{};
        if(::fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)||st.st_nlink!=1||st.st_size>65536){
            ::close(fd);return fail(QStringLiteral("Connector configuration must be an owned regular private file (0600), at most 64 KiB."));
        }
        QByteArray bytes;char buffer[4096];ssize_t n;
        while((n=::read(fd,buffer,sizeof buffer))>0){bytes.append(buffer,n);if(bytes.size()>65536)break;}
        struct stat after{};const bool stable=::fstat(fd,&after)==0&&after.st_size==st.st_size&&after.st_mtime==st.st_mtime;
        ::close(fd);
        if(n<0||!stable||bytes.size()>65536)return fail(QStringLiteral("Configuration changed while reading. Reload before saving."));
        QJsonParseError parse{};const auto doc=QJsonDocument::fromJson(bytes,&parse);
        if(parse.error!=QJsonParseError::NoError||!doc.isObject())return fail(QStringLiteral("Connector configuration is not a JSON object."));
        *out={true,doc.object(),bytes,quint64(st.st_dev),quint64(st.st_ino)};return true;
    }
#endif
};
}
