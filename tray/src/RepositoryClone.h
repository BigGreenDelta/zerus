#pragma once
#include <QObject>
#include <QJsonObject>
#include "MachinePlatform.h"
class HgsClient;

// One selected computer and one durable acquisition identity. Views share this
// workflow; reading an uncertain result never authorizes another clone or start.
class RepositoryClone : public QObject {
    Q_OBJECT
public:
    RepositoryClone(HgsClient *client,const QString &host,MachinePlatform::Style style,QObject *parent=nullptr);
    static QString suggestedName(const QString &url);
    QString host() const {return m_host;}
    QString url() const {return m_url;}
    QString parentFolder() const {return m_parent;}
    QString folderName() const {return m_name;}
    QString destination() const;
    QString createdPath() const {return m_created;}
    QString statusText() const {return m_status;}
    bool busy() const {return m_request!=0;}
    bool cloning() const {return busy()&&m_action=="clone";}
    bool uncertain() const {return m_uncertain;}
    bool fieldsLocked() const {return busy()||m_uncertain||!m_created.isEmpty();}
    bool canSubmit() const;
    void setSource(const QString &url,const QString &parent,const QString &name);
    void begin();
    void checkResult();
    void chooseAnotherDestination();
    void cancelCheck();
signals:
    void changed();
    void prepared(const QString &path);
private:
    QString settingsKey() const;
    void finish(quint64 request,bool ok,const QJsonObject &data,const QString &error);
    HgsClient *m_client;
    QString m_host,m_url,m_parent,m_name,m_created,m_status,m_token,m_action,m_expected;
    MachinePlatform::Style m_style;
    quint64 m_request=0;
    bool m_uncertain=false;
};
