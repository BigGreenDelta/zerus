#include "UpdateController.h"
#include "ChildProcess.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVersionNumber>
#include <QXmlStreamReader>
#include <cmath>
#include <memory>

namespace {
const QStringList packages={"zerus","zerus-git","zerus-ade-bin","zerus-ade-nightly-bin"};
const QStringList casks={"zerus","zerus@nightly","zerus@dev"};
int g_testCommandTimeoutMs=0; // 0: the 5 s deadline
const char *stableVersion="\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z";
const char *nightlyVersion="\\A[0-9]+\\.[0-9]+\\.[0-9]+\\.r[0-9]+\\.g[a-f0-9]{7}\\.n([0-9]{1,15})\\z";
const char *devVersion="\\A[0-9]+\\.[0-9]+\\.[0-9]+-dev\\.([1-9][0-9]{0,14})\\z";
// The newest stable release published without the Windows portable zip; every
// later stable release carries it, as nightly releases do. Stable publication
// enforces this (LAST_STABLE_WITHOUT_WINDOWS in scripts/release_common.py).
const char *lastStableWithoutWindows="0.42.2";
// The Windows Package Manager identifier of the stable portable zip (scripts/winget_manifest.py).
const char *wingetIdentifier="ufna.Zerus";
bool matches(const QString &s,const char *pattern){return QRegularExpression(QString::fromLatin1(pattern)).match(s).hasMatch();}
bool integer(const QJsonValue &v,qint64 low,qint64 high){double n=v.toDouble(-1);return v.isDouble()&&std::isfinite(n)&&std::floor(n)==n&&n>=low&&n<=high;}
bool safe(const QString &s){return matches(s,"\\A[A-Za-z0-9][A-Za-z0-9._+-]{0,179}\\z")&&!s.contains("..");}
QString configuredChannel(){QString c=QSettings().value("updates/channel","stable").toString();return c=="nightly"||c=="dev"?c:"stable";}
bool macosInstallation(const DesktopInstallation &installation){return installation.kind=="homebrew-cask"||installation.kind=="macos-bundle";}
// The portable zip (also through winget) and the per-user installer: Windows builds
// learn about newer ones from the feed and link their release pages.
bool windowsBuild(const DesktopInstallation &installation){return installation.kind=="portable"||installation.kind=="installer";}
bool parseMacos(const QJsonObject &raw,const QString &version,const QString &prefix,DesktopUpdateFeed &result){
    if(!raw.contains("macos"))return true;
    if(!raw["macos"].isObject())return false;
    const auto mac=raw["macos"].toObject();
    const auto archive=QString("zerus-%1-macos-arm64.zip").arg(version),image=QString("zerus-%1-macos-arm64.dmg").arg(version);
    const char *uuid="\\A[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}\\z";
    if(mac["asset"]!=archive||mac["minimum_system_version"]!="14.0"||!matches(mac["team_id"].toString(),"\\A[A-Z0-9]{10}\\z")||!matches(mac["qt_version"].toString(),stableVersion)||!matches(mac["notarization_id"].toString(),uuid))return false;
    QStringList names{archive};
    if(mac.contains("dmg")){
        if(!mac["dmg"].isObject())return false;const auto dmg=mac["dmg"].toObject();
        if(dmg.size()!=2||dmg["asset"]!=image||!matches(dmg["notarization_id"].toString(),uuid))return false;
        names<<image;
    }
    if(mac.size()!=(names.size()==2?7:6)||!mac["downloads"].isArray())return false;
    const auto downloads=mac["downloads"].toArray();if(downloads.size()!=names.size())return false;
    for(qsizetype i=0;i<names.size();++i){
        if(!downloads[i].isObject())return false;const auto row=downloads[i].toObject();
        if(row.size()!=4||row["name"]!=names[i]||row["url"]!=prefix+names[i]||!integer(row["size"],1,512*1024*1024)||!matches(row["sha256"].toString(),"\\A[a-f0-9]{64}\\z"))return false;
    }
    result.macosZipUrl=prefix+archive;if(names.size()==2)result.macosDmgUrl=prefix+image;
    return true;
}
QHash<QString,QString> bundleValues(const QString &path){
    QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>256*1024)return {};
    QXmlStreamReader xml(&file);QHash<QString,QString> values;
    if(!xml.readNextStartElement()||xml.name()!=QLatin1String("plist")||!xml.readNextStartElement()||xml.name()!=QLatin1String("dict"))return {};
    while(xml.readNextStartElement()){
        if(xml.name()!=QLatin1String("key"))return {};
        const auto key=xml.readElementText();if(values.contains(key)||!xml.readNextStartElement())return {};
        if(xml.name()==QLatin1String("string"))values.insert(key,xml.readElementText());
        else {values.insert(key,{});xml.skipCurrentElement();}
    }
    while(!xml.atEnd())xml.readNext();
    return xml.hasError()?QHash<QString,QString>():values;
}
DesktopUpdateFeed parseDev(const QByteArray &data){
    DesktopUpdateFeed result;
    if(data.size()>256*1024)return result;
    QJsonParseError error;auto doc=QJsonDocument::fromJson(data,&error);
    if(error.error!=QJsonParseError::NoError||!doc.isObject())return result;
    auto raw=doc.object();
    if(!integer(raw["schemaVersion"],2,2)||raw["product"]!="zerus"||raw["platform"]!="desktop"||raw["channel"]!="dev"||raw["channelTag"]!="dev"||raw.contains("sourceDirty")||!integer(raw["buildId"],1,9007199254740991LL)||!integer(raw["buildNumber"],1,9007199254740991LL))return result;
    auto id=raw["buildId"].toInteger();auto commit=raw["sourceCommit"].toString();auto version=raw["versionName"].toString();
    if(!matches(commit,"\\A[a-f0-9]{40}\\z")||version.size()>120||!matches(version,"\\A[0-9]+\\.[0-9]+\\.[0-9]+(?:-dev\\.[0-9]+)?\\z")||raw["buildUrl"].toString()!=QString("https://github.com/ufna/zerus/actions/runs/%1").arg(id))return result;
    auto published=raw["publishedAt"].toString();auto at=QDateTime::fromString(published,Qt::ISODateWithMs);if(!at.isValid())at=QDateTime::fromString(published,Qt::ISODate);
    if(published.size()>40||!matches(published,"(?:Z|[+-][0-9]{2}:[0-9]{2})\\z")||!at.isValid()||!raw["artifacts"].isArray())return result;
    auto artifacts=raw["artifacts"].toArray();if(artifacts.isEmpty()||artifacts.size()>16)return result;
    QStringList seen;int sources=0,packages=0;
    for(auto value:artifacts){
        if(!value.isObject())return result;auto item=value.toObject();auto name=item["name"].toString();auto kind=item["kind"].toString();
        if(!safe(name)||seen.contains(name)||item["url"].toString()!=QString("https://zerus.dev/downloads/dev-desktop-%1/%2").arg(id).arg(name)||!matches(item["sha256"].toString(),"\\A[a-f0-9]{64}\\z")||!integer(item["size"],1,512*1024*1024))return result;
        seen<<name;
        if(kind=="source"){if(item["os"]!="any"||item["arch"]!="source"||!name.endsWith("-source.tar.gz"))return result;result.sourceUrl=item["url"].toString();++sources;}
        else if(kind=="arch-package"){if(item["os"]!="linux"||item["arch"]!="x86_64"||!name.endsWith(".pkg.tar.zst"))return result;result.packageUrl=item["url"].toString();++packages;}
        else return result;
    }
    if(sources!=1||packages!=1)return result;
    const auto number=raw["buildNumber"].toInteger();
    const auto suffix=QRegularExpression("-dev\\.([0-9]+)\\z").match(version);
    if(suffix.hasMatch()&&suffix.captured(1)!=QString::number(number))return result;
    if(raw.contains("macos")&&(!matches(version,devVersion)||!raw["macos"].toObject().contains("dmg")))return result;
    if(!parseMacos(raw,version,QString("https://zerus.dev/downloads/dev-desktop-%1/").arg(id),result))return {};
    result.valid=true;result.releaseId=id;result.buildNumber=number;result.sourceCommit=commit;result.releaseUrl=raw["buildUrl"].toString();result.version=suffix.hasMatch()?version:version+QString("-dev.%1").arg(number);return result;
}
}

QJsonObject UpdateController::buildInfo(){
    QJsonObject info{{"version",QCoreApplication::applicationVersion()},
                     {"versionName",QCoreApplication::applicationVersion()},
                     {"devBuildNumber",0},{"sourceCommit",QString()},{"sourceDirty",QJsonValue()}};
#ifdef ZERUS_BUILD_SOURCE_COMMIT
    info["sourceCommit"]=QStringLiteral(ZERUS_BUILD_SOURCE_COMMIT);
    info["sourceDirty"]=bool(ZERUS_BUILD_SOURCE_DIRTY);
#endif
#ifdef ZERUS_DEV_BUILD_NUMBER
    info["devBuildNumber"]=qint64(ZERUS_DEV_BUILD_NUMBER);
    info["versionName"]=QStringLiteral(ZERUS_DEV_BASE_VERSION)+QString("-dev.%1").arg(qint64(ZERUS_DEV_BUILD_NUMBER));
#endif
    return info;
}
QString UpdateController::installedVersion(){
    const auto info=buildInfo();const auto version=info["versionName"].toString();
    if(info["devBuildNumber"].toInteger()>0||info["sourceCommit"].toString().isEmpty())return version;
    const auto source=info["sourceCommit"].toString().left(7);
    return info["sourceDirty"].toBool()?tr("%1 (local changes %2)").arg(version,source):QString("%1 (%2)").arg(version,source);
}
QString UpdateController::installedDisplayVersion() const{
    return macosInstallation(m_installation)&&!m_installation.version.isEmpty()?m_installation.version:installedVersion();
}
QString UpdateController::releaseChannel(const QString &version){
    if(matches(version,devVersion))return "dev";
    if(matches(version,nightlyVersion))return "nightly";
    return matches(version,stableVersion)?QStringLiteral("stable"):QString();
}
QString UpdateController::preferredChannel(const DesktopInstallation &installation){
    if(macosInstallation(installation))return releaseChannel(installation.version);
    return prefersNightly(installation)?QStringLiteral("nightly"):QStringLiteral("stable");
}
QUrl UpdateController::downloadUrl(const DesktopInstallation &installation,const DesktopUpdateFeed &feed,const QString &platform){
    if(!feed.valid||windowsBuild(installation))return {};
    if(platform=="macos"&&!feed.macosZipUrl.isEmpty())return QUrl(feed.macosDmgUrl.isEmpty()?feed.macosZipUrl:feed.macosDmgUrl);
    if(feed.buildNumber>0&&!macosInstallation(installation))return QUrl(installation.arch?feed.packageUrl:feed.sourceUrl);
    return {};
}
bool UpdateController::macosUpdateAvailable(const DesktopInstallation &installation,const DesktopUpdateFeed &feed){
    if(!feed.valid||feed.macosZipUrl.isEmpty()||!macosInstallation(installation))return false;
    const auto channel=releaseChannel(feed.version);
    if(channel.isEmpty()||channel!=releaseChannel(installation.version))return false;
    if(channel=="nightly")return nightlyIsNewer(installation.version,feed.version);
    if(channel=="dev"){
        const QRegularExpression pattern(QString::fromLatin1(devVersion));
        return pattern.match(feed.version).captured(1).toLongLong()>pattern.match(installation.version).captured(1).toLongLong();
    }
    return plainStableIsNewer(installation.version,feed.version);
}

DesktopUpdateFeed UpdateController::parseFeed(const QByteArray &data,const QString &channel){
    if(channel=="dev")return parseDev(data);
    DesktopUpdateFeed result;
    if(data.size()>256*1024||!(channel=="stable"||channel=="nightly"))return result;
    QJsonParseError error;auto doc=QJsonDocument::fromJson(data,&error);if(error.error!=QJsonParseError::NoError||!doc.isObject())return result;
    auto raw=doc.object();
    if(!integer(raw["schemaVersion"],1,1)||raw["product"]!="zerus"||raw["platform"]!="desktop"||raw["channel"]!=channel||raw.contains("sourceDirty")||!integer(raw["releaseId"],1,9007199254740991LL))return result;
    QString version=raw["versionName"].toString(),release=raw["releaseUrl"].toString();
    if(version.size()>120||!matches(raw["sourceCommit"].toString(),"\\A[a-f0-9]{40}\\z"))return result;
    if(channel=="stable"?!matches(version,"\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z"):!matches(version,"\\A[0-9]+\\.[0-9]+\\.[0-9]+\\.r[0-9]+\\.g[a-f0-9]{7}\\.n[0-9]+\\z"))return result;
    const QString prefix="https://github.com/ufna/zerus/releases/tag/";
    if(!release.startsWith(prefix))return result;QString tag=release.mid(prefix.size());
    if(!safe(tag)||(channel=="stable"?tag!="v"+version:!matches(tag,"\\Anightly-[0-9]+\\z")))return result;
    QString published=raw["publishedAt"].toString();auto at=QDateTime::fromString(published,Qt::ISODateWithMs);if(!at.isValid())at=QDateTime::fromString(published,Qt::ISODate);
    if(published.size()>40||!matches(published,"(?:Z|[+-][0-9]{2}:[0-9]{2})\\z")||!at.isValid())return result;
    if(!raw["artifacts"].isArray())return result;auto artifacts=raw["artifacts"].toArray();if(artifacts.isEmpty()||artifacts.size()>16)return result;
    QStringList seen;bool package=false;
    for(auto value:artifacts){if(!value.isObject())return result;auto artifact=value.toObject();QString name=artifact["name"].toString(),kind=artifact["kind"].toString();
        if(!safe(name)||seen.contains(name)||artifact["url"].toString()!="https://zerus.dev/downloads/"+tag+"/"+name||!matches(artifact["sha256"].toString(),"\\A[a-f0-9]{64}\\z")||!integer(artifact["size"],1,512*1024*1024))return result;
        seen<<name;
        if(kind=="source"){if(artifact["os"]!="any"||artifact["arch"]!="source"||!name.endsWith("-source.tar.gz"))return result;}
        else if(kind=="arch-package"||kind=="linux-archive"){if(artifact["os"]!="linux"||artifact["arch"]!="x86_64"||!name.endsWith(kind=="arch-package"?".pkg.tar.zst":"-arch-x86_64.tar.gz"))return result;package|=kind=="arch-package";}
        else return result;
    }
    if(!package)return result;
    QJsonObject versions;if(raw.contains("packages")){if(!raw["packages"].isObject())return result;versions=raw["packages"].toObject();for(auto it=versions.begin();it!=versions.end();++it)if(!packages.contains(it.key())||!it.value().isString()||!matches(it.value().toString(),"\\A(?:[0-9]+:)?[0-9][A-Za-z0-9.+_~:-]{0,119}-[0-9]+(?:\\.[0-9]+)*\\z"))return result;}
    if(!parseMacos(raw,version,"https://github.com/ufna/zerus/releases/download/"+tag+"/",result))return {};
    result.version=version;result.releaseUrl=release;result.sourceCommit=raw["sourceCommit"].toString();result.packages=versions;result.valid=true;result.releaseId=raw["releaseId"].toInteger();return result;
}

bool UpdateController::isArchLinux(const QByteArray &osRelease){
    if(osRelease.size()>8192)return false;
    QHash<QString,QString> values;
    for(const auto &line:QString::fromUtf8(osRelease).split('\n')){
        QString text=line.trimmed();if(text.isEmpty()||text.startsWith('#'))continue;
        int equal=text.indexOf('=');if(equal<1)continue;QString key=text.left(equal),value=text.mid(equal+1);
        if(key!="ID"&&key!="ID_LIKE")continue;if(values.contains(key))return false;
        if(value.startsWith('"')||value.startsWith('\'')){if(value.size()<2||value.back()!=value.front())return false;value=value.mid(1,value.size()-2);}
        if(!matches(value,"\\A[a-z0-9_-]+(?: [a-z0-9_-]+)*\\z"))return false;
        values[key]=value;
    }
    return values.value("ID")=="arch"||values.value("ID_LIKE").split(' ').contains("arch");
}
QString UpdateController::updateCommand(const DesktopInstallation &installation,const QString &channel){
    // winget carries stable releases only (scripts/publish-winget.py).
    if(installation.kind=="portable")return installation.package==wingetIdentifier&&channel=="stable"?QStringLiteral("winget upgrade --id ufna.Zerus --exact"):QString();
    if(installation.kind=="homebrew-cask"){
        const QString wanted=channel=="stable"?QStringLiteral("zerus"):channel=="nightly"?QStringLiteral("zerus@nightly"):channel=="dev"?QStringLiteral("zerus@dev"):QString();
        if(wanted.isEmpty()||!casks.contains(installation.package))return {};
        if(wanted==installation.package)return "brew upgrade --cask "+wanted;
        // The casks conflict: switching channels replaces one with the other.
        return "brew uninstall --cask "+installation.package+" && brew install --cask ufna/zerus/"+wanted;
    }
    if(channel=="dev")return {};
    if(installation.helper!="yay"&&installation.helper!="paru")return {};
    if(installation.kind=="package"){
        if(!packages.contains(installation.package))return {};
        return installation.helper+(installation.package=="zerus-git"?QStringLiteral(" -Syu --devel"):QStringLiteral(" -Syu ")+installation.package);
    }
    if(!installation.arch||installation.kind=="checking")return {};
    return installation.helper+QStringLiteral(" -Syu ")+(channel=="nightly"?QStringLiteral("zerus-ade-nightly-bin"):QStringLiteral("zerus-ade-bin"));
}
InstallationGuide UpdateController::installationGuide(const DesktopInstallation &installation,const QString &channel,const DesktopUpdateFeed &feed){
    InstallationGuide guide;
    if(installation.kind=="package")guide.ownership=tr("Package: %1\nInstalled package: %2").arg(installation.package,installation.version.isEmpty()?tr("unconfirmed"):installation.version);
    else if(installation.kind=="homebrew-cask")guide.ownership=tr("Homebrew cask: %1\nInstalled version: %2").arg(installation.package,installation.version);
    else if(installation.kind=="macos-bundle")guide.ownership=tr("macOS app installed from a download\nInstalled version: %1").arg(installation.version);
    else if(installation.kind=="source")guide.ownership=tr("Local build in a Git checkout; update and rebuild deliberately.");
    else if(installation.kind=="checking")guide.ownership=tr("Checking installation ownership…");
    else if(installation.kind=="portable"&&installation.package==wingetIdentifier)guide.ownership=tr("winget package %1\nPortable Windows build %2").arg(installation.package,QCoreApplication::applicationVersion());
    else if(installation.kind=="portable")guide.ownership=tr("Portable Windows build %1. Download newer builds from the release page.").arg(QCoreApplication::applicationVersion());
    else if(installation.kind=="installer")guide.ownership=tr("Windows build %1, installed for this user by the Zerus installer. It does not update itself yet; download newer builds from the release page.").arg(QCoreApplication::applicationVersion());
    else guide.ownership=tr("Local / manually installed build; package ownership unconfirmed.");
    if(installation.arch)guide.ownership+=tr("\nSystem: Arch-based Linux");
    if(macosInstallation(installation)){
        if(!feed.valid){guide.instructions=tr("Check for updates to see the available macOS build and update instructions.");return guide;}
        if(feed.macosZipUrl.isEmpty()){guide.instructions=tr("No macOS build is published in this channel yet. Choose another channel.");return guide;}
        if(installation.kind=="macos-bundle"){
            guide.instructions=feed.macosDmgUrl.isEmpty()?tr("Download and extract the ZIP."):tr("Download and open the DMG.");
            guide.instructions+=tr(" Quit Zerus from its menu, move the new Zerus app into Applications and confirm replacing it, then open Zerus again. Your agents and tmux sessions keep running.");return guide;
        }
    }
    const QString value=updateCommand(installation,channel);
    if(installation.kind=="homebrew-cask"){
        if(value.isEmpty())return guide;
        guide.command=tr("Homebrew: ")+value;guide.copyName=tr("Copy Homebrew command");
        guide.instructions=tr("Run this in Terminal. Homebrew replaces Zerus.app without quitting Zerus, agents or tmux; then choose Restart to finish update.");
        return guide;
    }
    if(windowsBuild(installation)){
        if(installation.package!=wingetIdentifier)return guide;
        if(value.isEmpty()){guide.instructions=tr("winget carries stable releases only. Choose the stable channel for its update command.");return guide;}
        guide.command=tr("winget: ")+value;guide.copyName=tr("Copy winget command");
        guide.instructions=tr("Quit Zerus from its notification area menu, then run this in PowerShell or Command Prompt. winget lists a release once its manifest is accepted, which can take a while after GitHub. Agents and local sessions keep running.");
        return guide;
    }
    const bool optional=installation.kind!="package";
    if(!value.isEmpty())guide.command=(optional?tr("Optional AUR installation: "):tr("Full system update: "))+value;
    guide.copyName=optional?tr("Copy optional installation command"):tr("Copy update command");
    if(!value.isEmpty())guide.instructions=optional?tr("This installs an AUR package; the current local build remains separate. Review the full system upgrade and your executable path."):tr("Run this through your AUR helper and review the full system upgrade before confirming.");
    else if(installation.arch)guide.instructions=tr("No supported update command is available for this installation. See the installation instructions.");
    return guide;
}
QString UpdateController::runningPlatform(){
#if defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return {};
#endif
}
QUrl UpdateController::installationInstructionsUrl(const DesktopInstallation &installation,const QString &platform){
    // README.md no longer has an Installation section; anchors are GitHub heading slugs.
    QUrl url(QStringLiteral("https://github.com/ufna/zerus/blob/main/docs/installation.md"));
    if(platform=="linux")url.setFragment(installation.arch?QStringLiteral("arch-linux"):QStringLiteral("local-installation"));
    else if(platform=="macos")url.setFragment(installation.kind=="macos-bundle"?QStringLiteral("macos-dmg"):QStringLiteral("macos-homebrew"));
    else if(platform=="windows")url.setFragment(QStringLiteral("windows"));
    return url;
}
bool UpdateController::plainStableIsNewer(const QString &installed,const QString &released){
    if(!matches(installed,"\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z")||!matches(released,"\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z"))return false;
    qsizetype installedEnd=0,releasedEnd=0;auto local=QVersionNumber::fromString(installed,&installedEnd);auto available=QVersionNumber::fromString(released,&releasedEnd);
    return installedEnd==installed.size()&&releasedEnd==released.size()&&local.segmentCount()==3&&available.segmentCount()==3&&QVersionNumber::compare(available,local)>0;
}
QString UpdateController::wingetPackage(const QString &executable){
    // ...\WinGet\Packages\ufna.Zerus_<source>\...: per-user under %LOCALAPPDATA%\Microsoft,
    // machine-wide under %ProgramFiles%. Identifiers compare case-insensitively.
    const auto parts=QString(executable).replace('\\','/').split('/',Qt::SkipEmptyParts);
    for(int index=2;index<parts.size();++index){
        if(parts[index-2].compare("WinGet",Qt::CaseInsensitive)||parts[index-1].compare("Packages",Qt::CaseInsensitive))continue;
        const QString prefix=QString::fromLatin1(wingetIdentifier)+'_';
        if(parts[index].size()>prefix.size()&&parts[index].startsWith(prefix,Qt::CaseInsensitive))return QString::fromLatin1(wingetIdentifier);
    }
    return {};
}
bool UpdateController::installerRecord(const QByteArray &data){
    if(data.isEmpty()||data.size()>installerRecordLimit)return false;
    QJsonParseError error;const auto document=QJsonDocument::fromJson(data,&error);
    if(error.error!=QJsonParseError::NoError||!document.isObject())return false;
    const auto record=document.object();
    return integer(record["schema"],1,1)&&record["kind"]=="installer"&&record["product"]=="Zerus";
}
bool UpdateController::stableHasWindowsBuild(const QString &version){return plainStableIsNewer(QString::fromLatin1(lastStableWithoutWindows),version);}
bool UpdateController::portableUpdateAvailable(const QString &appVersion,const DesktopUpdateFeed &feed){
    // The zip reports only its product version, so only a newer product version is
    // known to be newer. A nightly is <product>.r<count>.g<commit>.n<run>; a stable
    // release must also carry a Windows zip. Dev builds never do.
    if(!feed.valid||feed.buildNumber>0)return false;
    if(matches(feed.version,stableVersion))return stableHasWindowsBuild(feed.version)&&plainStableIsNewer(appVersion,feed.version);
    auto nightly=QRegularExpression("\\A([0-9]+\\.[0-9]+\\.[0-9]+)\\.r[0-9]+\\.g[a-f0-9]{7}\\.n[0-9]+\\z").match(feed.version);
    return nightly.hasMatch()&&plainStableIsNewer(appVersion,nightly.captured(1));
}
bool UpdateController::devBuildIsNewer(const DesktopUpdateFeed &feed,qint64 installedBuild,const QString &installedCommit){
    if(!feed.valid||feed.buildNumber<=0||feed.sourceCommit==installedCommit)return false;
    return installedBuild<=0||feed.buildNumber>installedBuild;
}
int UpdateController::automaticInterval(const QString &channel){return channel=="dev"?15*60:24*60*60;}
bool UpdateController::isUpdateToken(const QString &token){return matches(token,"\\Azerus-update:(?:v1:(?:stable|nightly)|v2:dev):[1-9][0-9]{0,15}\\z");}
bool UpdateController::claimNotification(const QString &token){
    if(!isUpdateToken(token))return false;QSettings settings;QString key="updates/notified/"+token;
    if(settings.value(key,false).toBool())return false;settings.setValue(key,true);settings.sync();return settings.status()==QSettings::NoError;
}
QString UpdateController::comparisonMessage(const DesktopInstallation &installation,const DesktopUpdateFeed &feed,const QString &appVersion,int comparison,bool compared){
    if(!feed.valid)return tr("No verified release catalog yet.");
    // Windows builds, portable or installed per user, do not update themselves.
    // Stable and nightly releases carry the zip; Dev builds do not.
    if(windowsBuild(installation)){
        if(feed.buildNumber>0)return tr("Dev builds do not include a Windows build yet. Choose the stable or nightly channel to hear about newer Windows builds.");
        if(matches(feed.version,stableVersion)){
            if(portableUpdateAvailable(appVersion,feed))return installation.package==wingetIdentifier?tr("Zerus %1 is available. Quit Zerus, then update it with winget.").arg(feed.version)
                                                                                         :tr("Zerus %1 is available. Download its Windows zip from the release page.").arg(feed.version);
            if(!stableHasWindowsBuild(feed.version)&&plainStableIsNewer(appVersion,feed.version))
                return tr("Zerus %1 has no Windows build; stable releases after it carry one. Until then, choose the nightly channel to hear about newer Windows builds.").arg(feed.version);
            return installation.kind=="installer"?tr("This Windows build does not update itself yet, and no newer stable release is published.")
                                                  :tr("Portable Windows build: there is no self-update, and no newer stable release is published.");
        }
        if(portableUpdateAvailable(appVersion,feed))return tr("Zerus nightly %1 is available. Download its Windows zip from the release page.").arg(feed.version);
        return installation.kind=="installer"?tr("This Windows build does not update itself yet, and nightly builds of the same version cannot be compared. The release page has the newest Windows zip.")
                                              :tr("Portable Windows build: there is no self-update, and nightly builds of the same version cannot be compared. The release page has the newest Windows zip.");
    }
    if(macosInstallation(installation)){
        if(feed.macosZipUrl.isEmpty())return tr("No macOS build is published in this channel yet. Choose another channel.");
        const QString channel=releaseChannel(feed.version),installed=releaseChannel(installation.version);
        if(channel!=installed){
            if(installation.kind=="homebrew-cask")return tr("This installation follows %1 builds. To switch to %2, run: %3").arg(installed,channel,updateCommand(installation,channel));
            return tr("This installation follows %1 builds. Download the %2 macOS app to switch channels.").arg(installed,channel);
        }
        if(!macosUpdateAvailable(installation,feed))return tr("Installed macOS build is at least as new as this release.");
        return installation.kind=="homebrew-cask"?tr("Update available. Run %1; the Homebrew tap may still be propagating.").arg(updateCommand(installation,channel))
                                                  :tr("Update available. Download the macOS app and replace Zerus in Applications.");
    }
    if(feed.buildNumber>0)return tr("Verified main build %1 is published. Use its source or platform package to update this installation.").arg(feed.buildNumber);
#ifdef Q_OS_MACOS
    Q_UNUSED(comparison);Q_UNUSED(compared);
    if(feed.macosZipUrl.isEmpty())return tr("No macOS build is published in this channel yet. This local build can be updated from source.");
    const QString cask=matches(feed.version,nightlyVersion)?QStringLiteral("brew install --cask ufna/zerus/zerus@nightly"):QStringLiteral("brew install --cask ufna/zerus/zerus");
    if(plainStableIsNewer(appVersion,feed.version))return tr("Update available for this local build. Rebuild it, or install the Homebrew cask: %1").arg(cask);
    return tr("Local build: release freshness is unconfirmed. Rebuild deliberately, or install the Homebrew cask: %1").arg(cask);
#else
    if(installation.kind=="package"){
        if(installation.package=="zerus-git")return tr("VCS package: check main through your AUR helper with --devel.");
        if(!compared)return tr("No matching package version comparison available.");
        return comparison>0?tr("Update available. AUR publication may still be propagating."):tr("Installed package is at least as new as this release.");
    }
    if(plainStableIsNewer(appVersion,feed.version))return tr("Update available for this local build.");
    return tr("Local build: release freshness is unconfirmed. Update or rebuild deliberately.");
#endif
}

DesktopInstallation UpdateController::homebrewInstallation(const QString &canonicalExecutable,const QStringList &prefixes){
    DesktopInstallation result;result.kind.clear();
    // Homebrew keeps one Caskroom/<token>/<version> directory per installed cask and moves
    // the application itself to the application directory.
    const QFileInfo executable(canonicalExecutable);const QDir macos=executable.absoluteDir();QDir bundle=macos;
    if(executable.fileName()!="hgs-tray"||macos.dirName()!="MacOS"||!bundle.cdUp()||bundle.dirName()!="Contents"||!bundle.cdUp()||bundle.dirName()!="Zerus.app")return result;
    for(const QString &prefix:prefixes){
        QStringList owners,versions;
        for(const QString &cask:casks){
            const QDir caskroom(prefix+"/Caskroom/"+cask);if(!caskroom.exists())continue;
            owners<<cask;
            for(const QString &name:caskroom.entryList(QDir::Dirs|QDir::NoDotAndDotDot))if(matches(name,cask=="zerus"?stableVersion:cask=="zerus@dev"?devVersion:nightlyVersion))versions<<name;
        }
        if(owners.isEmpty())continue;
        if(owners.size()==1&&versions.size()==1){
            const QFileInfo link(prefix+"/Caskroom/"+owners.first()+"/"+versions.first()+"/Zerus.app");
            if(link.isSymLink()&&link.canonicalFilePath()==bundle.canonicalPath()){
                result.kind="homebrew-cask";result.package=owners.first();result.version=versions.first();
            }
        }
        return result;
    }
    return result;
}
DesktopInstallation UpdateController::macosBundleInstallation(const QString &canonicalExecutable){
    DesktopInstallation result;result.kind.clear();
    const QFileInfo executable(canonicalExecutable);QDir directory=executable.absoluteDir();
    if(executable.fileName()!="hgs-tray"||directory.dirName()!="MacOS"||!directory.cdUp()||directory.dirName()!="Contents")return result;
    const auto values=bundleValues(directory.filePath("Info.plist"));
    const auto version=values.value("CFBundleShortVersionString"),release=values.value("ZerusReleaseVersion");
    if(values.value("CFBundleIdentifier")!="com.hgdev.hgs-tray"||values.value("CFBundleExecutable")!="hgs-tray"||values.value("CFBundleName")!="Zerus"||!matches(version,stableVersion)||releaseChannel(release).isEmpty())return result;
    const auto base=release.section("-dev.",0,0);
    if(release!=version&&!release.startsWith(version+".r")&&!(matches(release,devVersion)&&plainStableIsNewer(version,base)))return result;
    result.kind="macos-bundle";result.version=release;return result;
}
bool UpdateController::prefersNightly(const DesktopInstallation &installation){
    return installation.package=="zerus-ade-nightly-bin"||(installation.kind=="homebrew-cask"&&installation.package=="zerus@nightly");
}
bool UpdateController::nightlyIsNewer(const QString &installed,const QString &released){
    // The Nightly workflow run number only grows; the product version and revision may not.
    const QRegularExpression pattern(QString::fromLatin1(nightlyVersion));
    const auto local=pattern.match(installed),available=pattern.match(released);
    return local.hasMatch()&&available.hasMatch()&&available.captured(1).toLongLong()>local.captured(1).toLongLong();
}
QString UpdateController::bundleRelease(const QString &infoPlistPath){
    const auto values=bundleValues(infoPlistPath);
    const QString release=values.value("ZerusReleaseVersion"),version=values.value("CFBundleShortVersionString");
    if(!releaseChannel(release).isEmpty())return release;
    return matches(version,"\\A[0-9]+(?:\\.[0-9]+){1,3}\\z")?version:QString();
}
bool UpdateController::bundleReplaced(const QString &executable,const QString &startedRelease){
    const QDir macos=QFileInfo(executable).absoluteDir();if(macos.dirName()!="MacOS"||startedRelease.isEmpty())return false;
    const QString installed=bundleRelease(macos.absoluteFilePath("../Info.plist"));
    return !installed.isEmpty()&&installed!=startedRelease;
}
void UpdateController::refreshBundleState(){
#ifdef Q_OS_MACOS
    const bool replaced=bundleReplaced(m_executable,m_startedRelease);
    if(replaced!=m_restartRequired){m_restartRequired=replaced;emit changed();}
#endif
}
bool UpdateController::startRestartHelper(const QString &bundle,qint64 pid,const QString &shell){
    // The new instance must start after this one exits, or it would hand its request to
    // this process through the single-instance socket and exit.
    const QString script=QStringLiteral("while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; exec /usr/bin/open -a \"$2\" --args --sessions");
    // Detached, it outlives this process; it waits that long, so it must not hold a
    // request's pipe that ProcessRunner is still creating (ChildProcess.h).
    return ChildProcess::startDetached(shell,{QStringLiteral("-c"),script,QStringLiteral("zerus-restart"),QString::number(pid),bundle});
}
void UpdateController::restartIntoInstalledBundle(){
#ifdef Q_OS_MACOS
    QDir bundle=QFileInfo(m_executable).absoluteDir();if(!bundle.cdUp()||!bundle.cdUp()||!bundle.dirName().endsWith(".app"))return;
    if(startRestartHelper(bundle.absolutePath(),QCoreApplication::applicationPid()))QCoreApplication::quit();
#endif
}

UpdateController *UpdateController::instance(){static auto *value=new UpdateController(QCoreApplication::instance());return value;}
UpdateController::UpdateController(QObject *parent,QString executable,std::function<QNetworkReply *(const QNetworkRequest &)> fetch):QObject(parent),m_fetch(std::move(fetch)){
#ifdef Q_OS_LINUX
    QFile osRelease("/etc/os-release");if(osRelease.open(QIODevice::ReadOnly))m_installation.arch=isArchLinux(osRelease.read(8193));
    if(!QStandardPaths::findExecutable("yay").isEmpty())m_installation.helper="yay";else if(!QStandardPaths::findExecutable("paru").isEmpty())m_installation.helper="paru";
#endif
    auto *periodic=new QTimer(this);periodic->setInterval(15*60*1000);connect(periodic,&QTimer::timeout,this,&UpdateController::checkAutomatically);connect(periodic,&QTimer::timeout,this,&UpdateController::refreshBundleState);periodic->start();
    restoreCatalog();
    if(executable.isEmpty())executable=QCoreApplication::applicationFilePath();
    m_executable=executable;
#ifdef Q_OS_MACOS
    m_startedRelease=bundleRelease(QFileInfo(executable).absoluteDir().absoluteFilePath("../Info.plist"));
#endif
    QTimer::singleShot(0,this,[this,executable]{detectInstallation(executable);refreshBundleState();});
    connect(this,&UpdateController::changed,this,[this]{if(m_startCheck&&m_installation.kind!="checking"){m_startCheck=false;checkAutomatically();}});
}
QString UpdateController::channel() const{return configuredChannel();}
void UpdateController::setChannel(const QString &value){if(value!="stable"&&value!="nightly"&&value!="dev")return;++m_checkGeneration;m_automaticInFlight=false;if(m_activeReply)m_activeReply->abort();QSettings().setValue("updates/channel",value);m_freshForNotification=false;m_feed={};m_notice.clear();m_error.clear();m_comparison.clear();m_lastSuccess=0;m_cached=false;m_available=false;m_message="Updates have not been checked for this channel.";restoreCatalog();++m_checkGeneration;m_busy=false;emit changed();compare();checkAutomatically();}
bool UpdateController::automaticChecks() const{return QSettings().value("updates/automatic",true).toBool();}
void UpdateController::setAutomaticChecks(bool enabled){
    QSettings().setValue("updates/automatic",enabled);
    if(!enabled){m_startCheck=false;m_freshForNotification=false;if(m_automaticInFlight){++m_checkGeneration;m_busy=false;m_automaticInFlight=false;m_freshForNotification=false;if(m_activeReply)m_activeReply->abort();}}
    emit changed();if(enabled)checkAutomatically();
}
void UpdateController::checkOnStart(){if(!automaticChecks())return;m_startCheck=true;if(m_installation.kind!="checking")emit changed();}
void UpdateController::checkAutomatically(){
    if(!automaticChecks())return;if(m_installation.kind=="checking"){m_startCheck=true;return;}auto now=QDateTime::currentSecsSinceEpoch();auto last=QSettings().value("updates/lastAttempt/"+channel(),0).toLongLong();
    // Dev builds carry no Windows zip: a portable build has nothing to poll for.
    const int interval=windowsBuild(m_installation)?automaticInterval("stable"):automaticInterval(channel());if(last<=0||last>now||now-last>=interval)startCheck(true);
}
QString UpdateController::lastChecked() const{return m_lastSuccess>0?QLocale().toString(QDateTime::fromSecsSinceEpoch(m_lastSuccess).toLocalTime(),QLocale::ShortFormat):tr("Never");}
QString UpdateController::updateToken() const{return m_available&&m_feed.valid?(channel()=="dev"?"zerus-update:v2:":"zerus-update:v1:")+channel()+":"+QString::number(m_feed.releaseId):QString();}
void UpdateController::run(const QString &program,const QStringList &arguments,std::function<void(bool,QString)> done){
    auto *process=new QProcess(this);auto *deadline=new QTimer(process);deadline->setSingleShot(true);auto data=std::make_shared<QByteArray>();auto failed=std::make_shared<bool>(false);auto settled=std::make_shared<bool>(false);
    auto finish=[process,done,data,failed,settled](bool okay){if(*settled)return;*settled=true;done(okay&&!*failed,QString::fromUtf8(*data).trimmed());process->deleteLater();};
    connect(process,&QProcess::readyReadStandardOutput,this,[process,data,failed]{auto bytes=process->readAllStandardOutput();if(data->size()+bytes.size()>32768){*failed=true;process->kill();}else *data+=bytes;});
    connect(process,&QProcess::readyReadStandardError,this,[process]{process->readAllStandardError();});
    connect(process,&QProcess::finished,this,[finish,deadline](int code,QProcess::ExitStatus status){deadline->stop();finish(code==0&&status==QProcess::NormalExit);});
    connect(process,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(false);});
    connect(deadline,&QTimer::timeout,this,[process,failed]{*failed=true;process->kill();});
    // Like every child of the GUI, pacman and vercmp inherit only stdio (ChildProcess.h).
    process->setProgram(program);process->setArguments(arguments);ChildProcess::inheritStdioOnly(*process);process->start();deadline->start(g_testCommandTimeoutMs>0?g_testCommandTimeoutMs:5000);
}
void UpdateController::setCommandTimeoutForTests(int ms){g_testCommandTimeoutMs=ms;}
void UpdateController::sourceInstallation(const QString &executable){
    m_installation.kind="unknown";QDir directory=QFileInfo(executable).absoluteDir();
#ifdef Q_OS_WIN
    // The portable zip has build-info.json next to hgs-tray.exe: never a source
    // build, even unpacked inside a folder kept in Git (a profile, for instance).
    const bool bundle=QFileInfo(directory.filePath("build-info.json")).isFile();
#else
    const bool bundle=false;
#endif
    for(int depth=0;!bundle&&depth<20;++depth){if(QFileInfo::exists(directory.filePath(".git"))){m_installation.kind="source";break;}if(!directory.cdUp())break;}
#ifdef Q_OS_WIN
    // Windows builds ship as a portable zip; there is no package manager to ask.
    // Stable stays the default: stable releases after 0.42.2 carry the same tested
    // zip as nightlies. A channel saved by an earlier build, which defaulted to
    // nightly, is kept.
    // The per-user installer leaves installation.json next to hgs-tray.exe
    // (scripts/windows_installer.py); a zip has none. Neither updates itself.
    if(m_installation.kind=="unknown"){
        QFile record(QFileInfo(executable).absoluteDir().filePath("installation.json"));QByteArray data;
        if(record.open(QIODevice::ReadOnly))data=record.read(installerRecordLimit+1);
        if(installerRecord(data))m_installation.kind="installer";
        else{m_installation.kind="portable";m_installation.package=wingetPackage(executable);}
    }
#endif
    emit changed();compare();
}
void UpdateController::detectInstallation(const QString &executable){
    QString canonical=QFileInfo(executable).canonicalFilePath();if(canonical.isEmpty()){m_installation.kind="unknown";emit changed();compare();return;}
#ifdef Q_OS_MACOS
    auto mac=homebrewInstallation(canonical,{QStringLiteral("/opt/homebrew"),QStringLiteral("/usr/local")});
    if(mac.kind.isEmpty())mac=macosBundleInstallation(canonical);
    if(macosInstallation(mac)){
        m_installation=mac;
        if(!QSettings().contains("updates/channel")){QSettings().setValue("updates/channel",preferredChannel(m_installation));m_feed={};restoreCatalog();}
        emit changed();compare();return;
    }
#endif
#ifdef Q_OS_LINUX
    QString pacman=QStandardPaths::findExecutable("pacman");if(!pacman.isEmpty()){
        run(pacman,{"-Qqo","--",canonical},[this,pacman,canonical](bool okay,QString owner){
            if(!okay||!matches(owner,"\\A[A-Za-z0-9][A-Za-z0-9@._+-]{0,119}\\z")){sourceInstallation(canonical);return;}
            m_installation.kind="package";m_installation.package=owner;
            run(pacman,{"-Q","--",owner},[this,owner](bool success,QString raw){if(success&&raw.startsWith(owner+" ")&&!raw.contains('\n')){QString version=raw.mid(owner.size()+1);if(matches(version,"\\A[0-9][A-Za-z0-9.+_~:-]{0,159}\\z"))m_installation.version=version;}
                if(prefersNightly(m_installation)&&!QSettings().contains("updates/channel")){QSettings().setValue("updates/channel","nightly");m_feed={};restoreCatalog();}emit changed();compare();});
        });return;
    }
#endif
    sourceInstallation(canonical);
}
void UpdateController::check(){refreshBundleState();startCheck(false);}
void UpdateController::startCheck(bool automatic){
    if(automatic&&!automaticChecks())return;
    if(m_busy)return;m_busy=true;m_freshForNotification=false;m_automaticInFlight=automatic;m_message="Checking the published release catalog…";m_error.clear();emit changed();emit requestStarted();
    QString selected=channel();auto generation=++m_checkGeneration;QSettings().setValue("updates/lastAttempt/"+selected,QDateTime::currentSecsSinceEpoch());
    auto *manager=m_fetch?nullptr:new QNetworkAccessManager(this);QNetworkRequest request(QUrl("https://zerus.dev/updates/"+QString(selected=="dev"?"v2":"v1")+"/desktop/"+selected+".json"));request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);request.setTransferTimeout(10000);
    auto *reply=m_fetch?m_fetch(request):manager->get(request);m_activeReply=reply;auto data=std::make_shared<QByteArray>();auto oversized=std::make_shared<bool>(false);auto *timer=new QTimer(reply);timer->setSingleShot(true);connect(timer,&QTimer::timeout,reply,&QNetworkReply::abort);timer->start(10000);
    connect(reply,&QNetworkReply::readyRead,this,[reply,data,oversized]{auto bytes=reply->readAll();if(data->size()+bytes.size()>256*1024){*oversized=true;reply->abort();}else *data+=bytes;});
    connect(reply,&QNetworkReply::finished,this,[this,reply,manager,timer,data,oversized,generation,selected]{timer->stop();if(generation==m_checkGeneration){
        m_busy=false;m_activeReply=nullptr;bool mayNotify=!m_automaticInFlight||automaticChecks();m_automaticInFlight=false;DesktopUpdateFeed verified;if(!*oversized&&reply->error()==QNetworkReply::NoError&&reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==200)verified=parseFeed(*data,selected);
        if(verified.valid){m_freshForNotification=mayNotify;m_cached=false;m_available=false;m_feed=verified;m_lastSuccess=QDateTime::currentSecsSinceEpoch();QSettings settings;settings.setValue("updates/catalog/"+selected,*data);settings.setValue("updates/lastSuccess/"+selected,m_lastSuccess);m_notice="Last successful check: "+QDateTime::fromSecsSinceEpoch(m_lastSuccess).toString(Qt::ISODate)+".";}
        else {m_error=tr("Check failed. Try again later.");m_cached=m_feed.valid;m_notice=m_feed.valid?"Update check failed. Showing the last verified catalog ("+QDateTime::fromSecsSinceEpoch(m_lastSuccess).toString(Qt::ISODate)+").":"Update check failed or the catalog was invalid. Try again later.";}
        present(m_feed.valid?comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),0,false):QString());emit changed();compare();
    }reply->deleteLater();if(manager)manager->deleteLater();});
}
void UpdateController::completeComparison(bool newer,const QString &message){
    m_available=newer;present(message);emit changed();
    if(newer&&m_freshForNotification){QString token=updateToken();if(claimNotification(token))emit newerReleaseVerified(token,m_feed.version);}
}
void UpdateController::compare(){
    if(!m_feed.valid||m_installation.kind=="checking")return;
    if(macosInstallation(m_installation)){
        completeComparison(macosUpdateAvailable(m_installation,m_feed),comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),0,false));return;
    }
    if(channel()=="dev"){
        QString installedCommit;
#ifdef ZERUS_SOURCE_COMMIT
        installedCommit=QStringLiteral(ZERUS_SOURCE_COMMIT);
#endif
        completeComparison(!windowsBuild(m_installation)&&devBuildIsNewer(m_feed,buildInfo()["devBuildNumber"].toInteger(),installedCommit),comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),0,false));return;
    }
    if(m_installation.kind!="package"){
        const auto version=QCoreApplication::applicationVersion();
        bool newer=windowsBuild(m_installation)?portableUpdateAvailable(version,m_feed):channel()=="stable"&&plainStableIsNewer(version,m_feed.version);
        completeComparison(newer,comparisonMessage(m_installation,m_feed,version,0,false));return;
    }
    if(m_installation.package=="zerus-git"||m_installation.version.isEmpty()||!m_feed.packages.contains(m_installation.package)){completeComparison(false,comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),0,false));return;}
    QString vercmp=QStandardPaths::findExecutable("vercmp");if(vercmp.isEmpty()){completeComparison(false,comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),0,false));return;}
    auto generation=m_checkGeneration;run(vercmp,{m_feed.packages[m_installation.package].toString(),m_installation.version},[this,generation](bool okay,QString value){if(generation!=m_checkGeneration)return;bool parsed=false;int comparison=value.toInt(&parsed);bool verified=okay&&parsed&&matches(value,"\\A-?[0-9]+\\z");completeComparison(verified&&comparison>0,comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),comparison,verified));});
}

void UpdateController::present(const QString &message){m_comparison=message;m_message=m_notice+(m_notice.isEmpty()||message.isEmpty()?QString():QStringLiteral(" "))+message;}
void UpdateController::restoreCatalog(){
    QSettings settings;auto selected=channel();auto raw=settings.value("updates/catalog/"+selected).toByteArray();auto parsed=parseFeed(raw,selected);auto checked=settings.value("updates/lastSuccess/"+selected,0).toLongLong();
    if(!parsed.valid||checked<=0||checked>QDateTime::currentSecsSinceEpoch())return;
    m_feed=parsed;m_lastSuccess=checked;m_cached=true;m_notice="Last successful check: "+QDateTime::fromSecsSinceEpoch(checked).toString(Qt::ISODate)+" (cached).";
    present(comparisonMessage(m_installation,m_feed,QCoreApplication::applicationVersion(),0,false));
}
