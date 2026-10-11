#pragma once
#include <QObject>
#include <QJsonObject>
#include <QUrl>
#include <QPointer>
class QNetworkReply;
class QNetworkRequest;
#include <functional>

struct DesktopUpdateFeed {
    QString version, releaseUrl;
    QJsonObject packages;
    bool valid=false;
    qint64 releaseId=0;
};
struct DesktopInstallation {
    QString kind="checking", package, version;
    bool arch=false;
    QString helper;
};

class UpdateController : public QObject {
    Q_OBJECT
public:
    explicit UpdateController(QObject *parent=nullptr,QString executable={},std::function<QNetworkReply *(const QNetworkRequest &)> fetch={});
    static UpdateController *instance();
    static DesktopUpdateFeed parseFeed(const QByteArray &data,const QString &channel);
    static bool isArchLinux(const QByteArray &osRelease);
    static QString updateCommand(const DesktopInstallation &installation,const QString &channel="stable");
    static bool plainStableIsNewer(const QString &installed,const QString &released);
    // The portable Windows build (Phase 1: nightly zips only) against a catalog.
    static bool portableUpdateAvailable(const QString &appVersion,const DesktopUpdateFeed &feed);
    static bool claimNotification(const QString &token);
    static bool isUpdateToken(const QString &token);
    static QString comparisonMessage(const DesktopInstallation &installation,const DesktopUpdateFeed &feed,const QString &appVersion,int comparison,bool compared);
    QString channel() const;
    void setChannel(const QString &value);
    void check();
    void checkOnStart();
    void checkAutomatically();
    bool automaticChecks() const;
    void setAutomaticChecks(bool enabled);
    bool updateAvailable() const{return m_available;}
    QString updateToken() const;
    QString comparison() const{return m_comparison;}
    QString checkError() const{return m_error;}
    QString lastChecked() const;
    bool cached() const{return m_cached;}
    QString message() const{return m_message;}
    DesktopInstallation installation() const{return m_installation;}
    DesktopUpdateFeed feed() const{return m_feed;}
    bool busy() const{return m_busy;}
signals:
    void changed();
    void requestStarted();
    void newerReleaseVerified(const QString &token,const QString &version);
private:
    void detectInstallation(const QString &executable);
    void sourceInstallation(const QString &executable);
    void compare();
    void restoreCatalog();
    void present(const QString &message);
    void completeComparison(bool newer,const QString &message);
    void startCheck(bool automatic);
    void run(const QString &program,const QStringList &arguments,std::function<void(bool,QString)> done);
    DesktopInstallation m_installation;
    DesktopUpdateFeed m_feed;
    QString m_message="Updates have not been checked.",m_notice,m_comparison,m_error;
    bool m_available=false,m_cached=false,m_freshForNotification=false,m_automaticInFlight=false;
    QPointer<QNetworkReply> m_activeReply;
    std::function<QNetworkReply *(const QNetworkRequest &)> m_fetch;
    qint64 m_lastSuccess=0;
    bool m_busy=false,m_startCheck=false;
    quint64 m_checkGeneration=0;
};
